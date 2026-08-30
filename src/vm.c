// L^ (lhat) -- the machine: running compiled bytecode.

#include "lhat/vm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "environment.h"
#include "gc.h"
#include "hosted.h"
#include "lhat/config.h"
#include "machine.h"
// For LHAT_OPERATOR_MEMBERS alone: the one list of operator spellings lives
// with the tokens. Types and macros only -- nothing here calls the lexer,
// so a bytecode-only build still links no front end (02 の 14.17改2).
#include "lhat/token.h"
#include "operators.h"
// 02 の 14.17改2: tonumber reads 01 の 10 章's grammar. number.h is that
// grammar and nothing else -- no source, no token, no diagnostic -- so the
// machine carries it without carrying a front end.
#include "number.h"
#include "lhat/port.h"
// 04 の 2.7: where localerror^.CastFailure's one object lives.
#include "registry.h"
#include "type.h"

// ---------------------------------------------------------------------------
// Machine
// ---------------------------------------------------------------------------

// 02 の 11.8: an operator is a member whose name is the operator itself, and
// this is the spelling call_operator looks a candidate up by. Also reused by
// finish() to name a panicking instruction (04 の 11.6) for a host, which is
// where LHAT_BC_ASCAST answers too even though 11.6's as^ is not one of
// 11.8's overloadable operators and never reaches call_operator itself.
// 11.6改3 left it here: a mismatch is an answer now rather than a fault, but
// the instruction can still fault for want of room to build that answer in.
// NULL for every other instruction.
static const char *operator_name(LhatOpcode op, size_t *length)
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

// 02 の 14.8改: the three that can leave the integers. Answers false when the
// result does not fit, and then the caller redoes it in reals.
//
// Computed through the unsigned type: signed overflow is undefined in C11, so
// the wrapped value a check would have to look at cannot be obtained by
// letting it wrap. Unsigned wraps by definition, and converting back is
// implementation-defined rather than undefined -- every target this runs on
// is two's complement, which C23 now requires outright.
static bool add_exact(int64_t x, int64_t y, int64_t *out)
{
    uint64_t wrapped = (uint64_t)x + (uint64_t)y;
    int64_t sum = (int64_t)wrapped;
    // Overflow is exactly the case where both operands share a sign and the
    // result does not.
    if (((x ^ sum) & (y ^ sum)) < 0) {
        return false;
    }
    *out = sum;
    return true;
}

static bool subtract_exact(int64_t x, int64_t y, int64_t *out)
{
    uint64_t wrapped = (uint64_t)x - (uint64_t)y;
    int64_t difference = (int64_t)wrapped;
    if (((x ^ y) & (x ^ difference)) < 0) {
        return false;
    }
    *out = difference;
    return true;
}

static bool multiply_exact(int64_t x, int64_t y, int64_t *out)
{
    if (x == 0 || y == 0) {
        *out = 0;
        return true;
    }
    uint64_t wrapped = (uint64_t)x * (uint64_t)y;
    int64_t product = (int64_t)wrapped;
    // Dividing back is the check that needs no wider type. The extra guard is
    // INT64_MIN / -1, which is the one division that overflows.
    if (x == -1 && y == INT64_MIN) {
        return false;
    }
    if (product / x != y) {
        return false;
    }
    *out = product;
    return true;
}

// 5.1: the generic form checks. 02 の 14.8 makes number^ one type with two
// representations, so an operation stays in integers when both sides are and
// widens when one of them is real -- or when the integers no longer hold the
// answer (14.8改).
static bool arithmetic(LhatOpcode op, LhatValue left, LhatValue right,
                       LhatValue *out, LhatRunStatus *status)
{
    if (!lhat_is_number(left) || !lhat_is_number(right)) {
        *status = LHAT_RUN_TYPE_ERROR;
        return false;
    }

    bool exact = lhat_is_integer(left) && lhat_is_integer(right);
    double a = lhat_number_as_real(left);
    double b = lhat_number_as_real(right);

    switch (op) {
        case LHAT_BC_ADD: {
            int64_t whole = 0;
            *out = exact && add_exact(lhat_as_integer(left),
                                      lhat_as_integer(right), &whole)
                       ? lhat_integer(whole)
                       : lhat_real(a + b);
            return true;
        }
        case LHAT_BC_SUB: {
            int64_t whole = 0;
            *out = exact && subtract_exact(lhat_as_integer(left),
                                           lhat_as_integer(right), &whole)
                       ? lhat_integer(whole)
                       : lhat_real(a - b);
            return true;
        }
        case LHAT_BC_MUL: {
            int64_t whole = 0;
            *out = exact && multiply_exact(lhat_as_integer(left),
                                           lhat_as_integer(right), &whole)
                       ? lhat_integer(whole)
                       : lhat_real(a * b);
            return true;
        }
        case LHAT_BC_DIV:
            // 04 の 11.2: real division, so a zero divisor gives inf rather
            // than failing. That is what keeps ordinary arithmetic out of the
            // unions.
            *out = lhat_real(a / b);
            return true;
        case LHAT_BC_IDIV:
        case LHAT_BC_MOD: {
            // 04 の 11.2: a zero divisor does not fail -- like an
            // overflow (14.8改), it widens to real arithmetic instead,
            // which already answers inf/nan for this the same way '/'
            // does, so ordinary arithmetic stays out of a union either way.
            if (exact && b != 0) {
                int64_t x = lhat_as_integer(left);
                int64_t y = lhat_as_integer(right);
                int64_t quotient = x / y;
                int64_t remainder = x % y;
                // Floor rather than truncate, so that '%' agrees in sign with
                // the divisor as it does in Lua.
                if (remainder != 0 && ((remainder < 0) != (y < 0))) {
                    quotient--;
                    remainder += y;
                }
                *out = op == LHAT_BC_IDIV ? lhat_integer(quotient)
                                          : lhat_integer(remainder);
                return true;
            }
            // floor() rather than a hand-rolled truncate-and-adjust: the
            // quotient can be inf or nan here (b == 0), and casting either
            // to int64_t is undefined.
            double floored = floor(a / b);
            *out = op == LHAT_BC_IDIV ? lhat_real(floored)
                                      : lhat_real(a - floored * b);
            return true;
        }
        case LHAT_BC_POW:
            if (lhat_is_integer(right)) {
                // small interger exponent specified hand loop
                int64_t times = lhat_as_integer(right);
                if (times == 0) {
                    *out = lhat_integer(1);
                    return true;
                }
                // small positive interger exponent specified hand loop
                if (lhat_is_integer(left) && times > 0 && times <= 100) {
                    int64_t r = 1;
                    int64_t base = lhat_as_integer(left);
                    if(base == 0) {
                        *out = lhat_integer(0);
                        return true;
                    }
                    for (int64_t i = 0; i < times; i++) {
                        r *= base;
                    }
                    *out = lhat_integer(r);
                    return true;
                }
                // generic small interger exponent hand loop
                if(fabs(a) > 0.00000001 && times <= 100 && times >= -100) {
                    double r = 1.0;
                    double base = a;
                    // Kept out of libm so the core has no maths dependency yet; only
                    // whole exponents are wanted before the standard library lands.
                    bool invert = times < 0;
                    if (invert)
                    {
                        times = -times;
                    }
                    for (int64_t i = 0; i < times; i++)
                    {
                        r *= base;
                    }
                    *out = lhat_real(invert ? 1.0 / r : r);
                    return true;
                }
            }
            // A negative base under a fractional exponent is NaN -- not a
            // fault, the same line 04 の 11.2 draws for a zero divisor.
            *out = lhat_real(pow(a, b));
            return true;
        default:
            *status = LHAT_RUN_TYPE_ERROR;
            return false;
    }
}

// 02 の 11.9: which of the two comes first, for the types that order
// their own. Negative, zero or positive -- the shape every '<=>' answers in,
// and what the four orderings read against zero. False when neither number^
// nor string^ is what is being asked about, which sends the question to the
// member 11.8 names.
static bool three_way(LhatValue left, LhatValue right, int *out)
{
    if (lhat_is_number(left) && lhat_is_number(right)) {
        // 14.8: one type over integers and reals, and the same question '='
        // asks -- so two numbers within the error a real carries order as
        // one. Reading '=' off '<=>' the way 11.9 does then answers what '='
        // answers, rather than 'equal' and 'less' both holding.
        if (lhat_value_close(left, right, LHAT_NUMBER_TOLERANCE)) {
            *out = 0;
            return true;
        }
        *out = lhat_number_as_real(left) < lhat_number_as_real(right) ? -1 : 1;
        return true;
    }
    if (lhat_is_object_kind(left, LHAT_OBJECT_STRING) &&
        lhat_is_object_kind(right, LHAT_OBJECT_STRING)) {
        // The bytes, in order. 01 の 1 章 fixes the encoding at UTF-8, which
        // orders code points the same way their bytes order.
        const LhatString *a = (const LhatString *)lhat_as_object(left);
        const LhatString *b = (const LhatString *)lhat_as_object(right);
        size_t shorter = a->length < b->length ? a->length : b->length;
        int by_bytes = shorter > 0 ? memcmp(a->text, b->text, shorter) : 0;
        if (by_bytes != 0) {
            *out = by_bytes < 0 ? -1 : 1;
        } else if (a->length == b->length) {
            *out = 0;
        } else {
            *out = a->length < b->length ? -1 : 1;
        }
        return true;
    }
    return false;
}

// 11.9: every ordering is read off a three-way answer, whether the
// answer came from a type that orders its own or from a written op^<=>. False
// when neither side is one of those, which is what sends the question on.
static bool ordering(LhatOpcode op, LhatValue left, LhatValue right,
                     bool *out, LhatRunStatus *status)
{
    int outcome = 0;
    if (!three_way(left, right, &outcome)) {
        *status = LHAT_RUN_TYPE_ERROR;
        return false;
    }
    switch (op) {
        case LHAT_BC_LT: *out = outcome <  0; return true;
        case LHAT_BC_LE: *out = outcome <= 0; return true;
        case LHAT_BC_GT: *out = outcome >  0; return true;
        case LHAT_BC_GE: *out = outcome >= 0; return true;
        default:
            *status = LHAT_RUN_TYPE_ERROR;
            return false;
    }
}

// What an index reads from. 04 の 2.3 gives every error message and cause
// without declaring them, so an error answers a member the same way a table
// does -- from the table its fields live in.
static LhatTable *table_of(LhatValue value)
{
    if (lhat_is_object_kind(value, LHAT_OBJECT_TABLE)) {
        return (LhatTable *)lhat_as_object(value);
    }
    if (lhat_is_object_kind(value, LHAT_OBJECT_ERROR)) {
        return ((LhatError *)lhat_as_object(value))->fields;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// 05 の 8.9: host values at run time
// ---------------------------------------------------------------------------

// The members table the machine built for a host value type at install, or
// NULL when the registration never reached this machine.
static LhatTable *hostvalue_members_of(Machine *m, const LhatHostValueTag *tag)
{
    return tag != NULL && tag->index < m->hostvalue_member_count
               ? m->hostvalue_members[tag->index]
               : NULL;
}

// Value equality is byte equality under the same tag -- a host value has no
// identity to fall back on, which is half of what makes it a value. Every
// making zeroes the data run's unused tail, so whole slots compare exactly.
static bool hostvalue_equal(LhatSlots slots, size_t left, size_t right)
{
    const LhatHostValueTag *tag = slots.values[left].hostvalue;
    if (tag == NULL || slots.tags[right] != LHAT_VALUE_HOSTVALUE ||
        slots.values[right].hostvalue != tag) {
        return false;
    }
    return memcmp(slots.values + left + 1, slots.values + right + 1,
                  (tag->width - 1) * sizeof(LhatValueUnion)) == 0;
}

// A host answered with a head-shaped run -- an argument passed through, or
// the scratch lhat_make_hostvalue filled -- written out whole into the slots
// at `at`. False when the run is not one.
static bool place_hostvalue_answer(Machine *m, size_t at, LhatValue answered)
{
    const LhatValueUnion *run = answered.as.hostvalue_run;
    const LhatHostValueTag *tag = run != NULL ? run[0].hostvalue : NULL;
    if (tag == NULL || at + tag->width > LHAT_STACK_SLOTS) {
        return false;
    }
    // Ascending, which survives the one overlapping case there is: a host
    // echoing an argument back, whose run sits above the answer slot.
    m->slots.values[at] = run[0];
    m->slots.tags[at] = (uint8_t)LHAT_VALUE_HOSTVALUE;
    for (size_t i = 1; i < tag->width; i++) {
        m->slots.values[at + i] = run[i];
        m->slots.tags[at + i] = (uint8_t)LHAT_VALUE_CONT;
    }
    return true;
}

// 05 の 8.9改2: the value a host receives from a yield^ or a return^ at the
// base of a run -- the bytes moved into the machine's scratch (the frame
// that held them is going) and the head re-aimed there. Alive until the
// next call that runs the machine, which is the tuple positions' contract;
// a host that keeps it longer copies the bytes out (lhat_hostvalue_data).
static LhatValue hand_hostvalue_out(Machine *m, LhatValue value)
{
    const LhatValueUnion *run = value.as.hostvalue_run;
    const LhatHostValueTag *tag = run != NULL ? run[0].hostvalue : NULL;
    if (tag == NULL) {
        return lhat_nil();
    }
    for (size_t i = 0; i < tag->width; i++) {
        m->hostvalue_scratch[i] = run[i];
    }
    value.as.hostvalue_run = m->hostvalue_scratch;
    return value;
}

// 02 の 13.8改: the several values a host answered with, laid down at `at`
// as the run every other producer makes -- head slot naming the width, the
// positions after it. `reserved` is what the call site left room for (CALL's
// C), and it has to agree: a host and its caller can disagree only by being
// compiled against different registrations, and quietly writing the wrong
// count would leave the caller reading slots nobody wrote.
//
// The room is released here, which is what keeps one enough: the next host
// call finds it free. Answers false when the widths disagree or the run
// would not fit.
static bool place_run_answer(Machine *m, size_t at, size_t reserved,
                             LhatValue answered)
{
    size_t positions = lhat_run_width(answered);
    size_t held = m->tuple_scratch_count;
    m->tuple_scratch_count = 0;
    if (positions != held || reserved != positions + 1 ||
        at + positions >= LHAT_STACK_SLOTS) {
        return false;
    }
    lhat_slots_set(m->slots, at, answered);
    for (size_t i = 0; i < positions; i++) {
        lhat_slots_set(m->slots, at + 1 + i, m->tuple_scratch[i]);
    }
    return true;
}

// A resume's sent host value, moved into the machine's scratch before the
// suspension's registers are restored over the very slots that hold it --
// the pointer form has to aim somewhere the restore cannot reach.
static LhatValue stash_sent_hostvalue(Machine *m, size_t slot)
{
    const LhatHostValueTag *tag = m->slots.values[slot].hostvalue;
    for (size_t i = 0; i < tag->width; i++) {
        m->hostvalue_scratch[i] = m->slots.values[slot + i];
    }
    LhatValue v;
    v.tag = LHAT_VALUE_HOSTVALUE;
    v.as.hostvalue_run = m->hostvalue_scratch;
    return v;
}

// The registered field a string key names, or NULL.
static const LhatHostValueField *hostvalue_field_named(
    const LhatHostValueTag *tag, LhatValue key)
{
    if (!lhat_is_object_kind(key, LHAT_OBJECT_STRING)) {
        return NULL;
    }
    const LhatString *name = (const LhatString *)lhat_as_object(key);
    for (size_t i = 0; i < tag->field_count; i++) {
        if (strlen(tag->fields[i].name) == name->length &&
            memcmp(tag->fields[i].name, name->text, name->length) == 0) {
            return &tag->fields[i];
        }
    }
    return NULL;
}

// 'v.x' as a number^, off the data run that follows a head. Offsets were
// checked against the registered size when the field was. Public so the
// value writer spells a host value's content with it (value.c).
LhatValue lhat_hostvalue_field_value(const LhatValueUnion *data,
                                     const LhatHostValueField *field)
{
    const uint8_t *bytes = (const uint8_t *)data + field->offset;
    switch (field->kind) {
        case LHAT_HVFIELD_F32: {
            float f;
            memcpy(&f, bytes, sizeof f);
            return lhat_real((double)f);
        }
        case LHAT_HVFIELD_F64: {
            double d;
            memcpy(&d, bytes, sizeof d);
            return lhat_real(d);
        }
        case LHAT_HVFIELD_I8: {
            int8_t i;
            memcpy(&i, bytes, sizeof i);
            return lhat_integer(i);
        }
        case LHAT_HVFIELD_I16: {
            int16_t i;
            memcpy(&i, bytes, sizeof i);
            return lhat_integer(i);
        }
        case LHAT_HVFIELD_I32: {
            int32_t i;
            memcpy(&i, bytes, sizeof i);
            return lhat_integer(i);
        }
        case LHAT_HVFIELD_I64: {
            int64_t i;
            memcpy(&i, bytes, sizeof i);
            return lhat_integer(i);
        }
        case LHAT_HVFIELD_U8: {
            uint8_t u;
            memcpy(&u, bytes, sizeof u);
            return lhat_integer(u);
        }
        case LHAT_HVFIELD_U16: {
            uint16_t u;
            memcpy(&u, bytes, sizeof u);
            return lhat_integer(u);
        }
        case LHAT_HVFIELD_U32: {
            uint32_t u;
            memcpy(&u, bytes, sizeof u);
            return lhat_integer(u);
        }
    }
    return lhat_nil();
}

// And the write. False when the value is not a number^ -- 14.8's one type,
// either representation.
static bool hostvalue_field_set(LhatValueUnion *data,
                                const LhatHostValueField *field,
                                LhatValue value)
{
    if (!lhat_is_number(value)) {
        return false;
    }
    uint8_t *bytes = (uint8_t *)data + field->offset;
    double real = lhat_number_as_real(value);
    int64_t integer = lhat_is_integer(value) ? lhat_as_integer(value)
                                             : (int64_t)real;
    switch (field->kind) {
        case LHAT_HVFIELD_F32: {
            float f = (float)real;
            memcpy(bytes, &f, sizeof f);
            return true;
        }
        case LHAT_HVFIELD_F64:
            memcpy(bytes, &real, sizeof real);
            return true;
        case LHAT_HVFIELD_I8: {
            int8_t i = (int8_t)integer;
            memcpy(bytes, &i, sizeof i);
            return true;
        }
        case LHAT_HVFIELD_I16: {
            int16_t i = (int16_t)integer;
            memcpy(bytes, &i, sizeof i);
            return true;
        }
        case LHAT_HVFIELD_I32: {
            int32_t i = (int32_t)integer;
            memcpy(bytes, &i, sizeof i);
            return true;
        }
        case LHAT_HVFIELD_I64:
            memcpy(bytes, &integer, sizeof integer);
            return true;
        case LHAT_HVFIELD_U8: {
            uint8_t u = (uint8_t)integer;
            memcpy(bytes, &u, sizeof u);
            return true;
        }
        case LHAT_HVFIELD_U16: {
            uint16_t u = (uint16_t)integer;
            memcpy(bytes, &u, sizeof u);
            return true;
        }
        case LHAT_HVFIELD_U32: {
            uint32_t u = (uint32_t)integer;
            memcpy(bytes, &u, sizeof u);
            return true;
        }
    }
    return false;
}

// 13.7: the i-th effective argument of a call, whether it sits in an
// ordinary register or comes from what a spread ('expr...') unpacked -- the
// two are not contiguous in memory, so nothing beyond this reads registers
// directly once a spread is in play.
//
// 13.8改: the spread is either a table's positions or a tuple's. A tuple's
// are contiguous with the registers -- they are the run's own slots, one
// past the head -- so `run_at` is where that head sits and the positions
// follow it. Only one of the two is ever set.
static LhatValue call_arg(LhatSlots regs, size_t rbase, uint8_t a, size_t skip,
                          const LhatTable *spread, size_t run_at,
                          size_t before_spread, size_t i)
{
    if (i >= before_spread) {
        if (spread != NULL) {
            return lhat_slots_get(spread->array, i - before_spread);
        }
        if (run_at != SIZE_MAX) {
            return lhat_slots_get(regs, run_at + 1 + (i - before_spread));
        }
    }
    return lhat_slots_get(regs, rbase + a + skip + i);
}

// The same question for a read. 05 の 8.8: what a host value carries is the
// registered type's table, so 't.width()' finds the member there -- but the
// table belongs to the type and not to the value, so a write must not reach
// it. That is why this is separate from table_of rather than part of it.
static const LhatTable *readable_table(LhatValue value)
{
    if (lhat_is_object_kind(value, LHAT_OBJECT_HOSTDATA)) {
        return ((const LhatHostData *)lhat_as_object(value))->members;
    }
    return table_of(value);
}

// 02 の 14.16: what typeof^ answers where no checked type was
// compiled in -- the value's TAG, the dispatch information every value
// already carries, read in O(1). It never walks a structure: a table is
// t^ whatever it holds (the deep answer is the checker's to give, at
// compile time), and a subroutine or coroutine answers from the types its
// proto already carries, which were made at compile time too (03 の 5.11a).
// The one deep-looking case, an overload's arms, is bounded by the arm
// count rather than by any value's size.
static LhatRuntimeType *tag_type(LhatHeap *heap, LhatValue value)
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
            LhatRuntimeType *arm = tag_type(heap, overload->candidates[i]);
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
    *hatted = false;
    if (name->length == 5 && memcmp(name->text, "start", 5) == 0) {
        *out = LHAT_NATIVE_START;
        return true;
    }
    if (name->length == 6 && memcmp(name->text, "resume", 6) == 0) {
        *out = LHAT_NATIVE_RESUME;
        return true;
    }
    if (name->length == 7 && memcmp(name->text, "dispose", 7) == 0) {
        *out = LHAT_NATIVE_DISPOSE;
        return true;
    }
    if (name->length == 4 && memcmp(name->text, "done", 4) == 0) {
        *out = LHAT_NATIVE_DONE;
        return true;
    }
    if (name->length == 7 && memcmp(name->text, "started", 7) == 0) {
        *out = LHAT_NATIVE_STARTED;
        return true;
    }
    if ((name->length == 7 && memcmp(name->text, "iterate", 7) == 0) ||
        (name->length == 8 && memcmp(name->text, "iterate^", 8) == 0)) {
        *out = LHAT_NATIVE_ITERATE;
        *hatted = name->length == 8;
        return true;
    }
    if ((name->length == 8 && memcmp(name->text, "tostring", 8) == 0) ||
        (name->length == 9 && memcmp(name->text, "tostring^", 9) == 0)) {
        *out = LHAT_NATIVE_TOSTRING;
        *hatted = name->length == 9;
        return true;
    }
    // 14.17改2 with 14.18改: what only a string^ carries has no hat spelling
    // at all. The hat is there to keep a built-in off a name the writer may
    // mean for something else, and nothing can be written on a string^ -- so
    // spelling it with one would be a second way of writing one member, which
    // is what the coroutine's start and resume never had either.
    if (name->length == 8 && memcmp(name->text, "tonumber", 8) == 0) {
        *out = LHAT_NATIVE_TONUMBER;
        return true;
    }
    // 14.20: a number^'s own comparison, with the error term written down.
    // No hat spelling, for 14.18改's reason -- nothing can be written on a
    // number^, so there is nothing for a hat to keep it clear of.
    if (name->length == 2 && memcmp(name->text, "eq", 2) == 0) {
        *out = LHAT_NATIVE_EQ;
        return true;
    }
    // 14.21: the three roundings, under the names every other language calls
    // them by. No hat spelling either, for the same reason as eq.
    if (name->length == 5 && memcmp(name->text, "floor", 5) == 0) {
        *out = LHAT_NATIVE_FLOOR;
        return true;
    }
    if (name->length == 4 && memcmp(name->text, "ceil", 4) == 0) {
        *out = LHAT_NATIVE_CEIL;
        return true;
    }
    if (name->length == 5 && memcmp(name->text, "round", 5) == 0) {
        *out = LHAT_NATIVE_ROUND;
        return true;
    }
    // 14.21改: and the three more, the same way.
    if (name->length == 3 && memcmp(name->text, "abs", 3) == 0) {
        *out = LHAT_NATIVE_ABS;
        return true;
    }
    if (name->length == 4 && memcmp(name->text, "sign", 4) == 0) {
        *out = LHAT_NATIVE_SIGN;
        return true;
    }
    if (name->length == 5 && memcmp(name->text, "clamp", 5) == 0) {
        *out = LHAT_NATIVE_CLAMP;
        return true;
    }
    // 14.19: one member under three names.
    if ((name->length == 9 && memcmp(name->text, "substring", 9) == 0) ||
        (name->length == 6 && memcmp(name->text, "substr", 6) == 0) ||
        (name->length == 3 && memcmp(name->text, "sub", 3) == 0)) {
        *out = LHAT_NATIVE_SUBSTRING;
        return true;
    }
    // 14.19改: one character of it.
    if (name->length == 2 && memcmp(name->text, "at", 2) == 0) {
        *out = LHAT_NATIVE_AT;
        return true;
    }
    // 14.19改3: the plain searches, bare for 14.18改's reason.
    if (name->length == 4 && memcmp(name->text, "find", 4) == 0) {
        *out = LHAT_NATIVE_FIND;
        return true;
    }
    if (name->length == 7 && memcmp(name->text, "findall", 7) == 0) {
        *out = LHAT_NATIVE_FINDALL;
        return true;
    }
    if (name->length == 7 && memcmp(name->text, "replace", 7) == 0) {
        *out = LHAT_NATIVE_REPLACE;
        return true;
    }
    if (name->length == 5 && memcmp(name->text, "split", 5) == 0) {
        *out = LHAT_NATIVE_SPLIT;
        return true;
    }
    if (name->length == 7 && memcmp(name->text, "toupper", 7) == 0) {
        *out = LHAT_NATIVE_TOUPPER;
        return true;
    }
    if (name->length == 7 && memcmp(name->text, "tolower", 7) == 0) {
        *out = LHAT_NATIVE_TOLOWER;
        return true;
    }
    // 16.3改2: the hat is not optional on these two (14.18's line), but the
    // bare spelling is still read here so builtin_member can refuse it by
    // name rather than by falling through to "no such member".
    if ((name->length == 4 && memcmp(name->text, "keys", 4) == 0) ||
        (name->length == 5 && memcmp(name->text, "keys^", 5) == 0)) {
        *out = LHAT_NATIVE_KEYS;
        *hatted = name->length == 5;
        return true;
    }
    if ((name->length == 6 && memcmp(name->text, "values", 6) == 0) ||
        (name->length == 7 && memcmp(name->text, "values^", 7) == 0)) {
        *out = LHAT_NATIVE_VALUES;
        *hatted = name->length == 7;
        return true;
    }
    // 02 の 14.22: the table's own operations. The bare spellings are read
    // here so builtin_member can refuse them by name -- on a plain table the
    // hat is what keeps a built-in off a word the writer may mean.
    {
        static const struct {
            const char *word;
            LhatNativeKind kind;
        } table_words[] = {
            { "join", LHAT_NATIVE_JOIN },
            { "indexof", LHAT_NATIVE_INDEXOF },
            { "contains", LHAT_NATIVE_CONTAINS },
            { "slice", LHAT_NATIVE_SLICE },
            { "clone", LHAT_NATIVE_CLONE },
            { "insert", LHAT_NATIVE_INSERT },
            { "push", LHAT_NATIVE_PUSH },
            { "extend", LHAT_NATIVE_EXTEND },
            { "remove", LHAT_NATIVE_REMOVE },
            { "pop", LHAT_NATIVE_POP },
            { "sort", LHAT_NATIVE_SORT },
            { "stablesort", LHAT_NATIVE_STABLESORT },
            { "move", LHAT_NATIVE_MOVE },
            { "reverse", LHAT_NATIVE_REVERSE },
            { "clear", LHAT_NATIVE_CLEAR },
        };
        for (size_t i = 0; i < sizeof table_words / sizeof table_words[0];
             i++) {
            size_t n = strlen(table_words[i].word);
            if ((name->length == n &&
                 memcmp(name->text, table_words[i].word, n) == 0) ||
                (name->length == n + 1 && name->text[n] == '^' &&
                 memcmp(name->text, table_words[i].word, n) == 0)) {
                *out = table_words[i].kind;
                *hatted = name->length == n + 1;
                return true;
            }
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

// 02 の 14.21: the whole number `toward` picks -- floor, ceil or nearbyint.
//
// 14.8改: an integer while it can be one. Past what an int64 names, the real
// is already whole (every double that large is), so it answers as itself --
// and an infinity or a NaN falls out of the same test rather than wanting one
// of its own, since neither is inside the range.
//
// nearbyint reads the rounding mode, whose default is to nearest with a half
// going to the even side. printf's "%.0f" reads the same mode, which is why
// 14.21's round and 14.17's format agree on a half -- one setting, not two
// implementations that happen to match.
static LhatValue whole_of(LhatValue value, double (*toward)(double))
{
    if (lhat_is_integer(value)) {
        return value;  // already the whole number it names
    }
    double whole = toward(lhat_as_real(value));
    if (whole >= -9223372036854775808.0 && whole < 9223372036854775808.0) {
        return lhat_integer((int64_t)whole);
    }
    return lhat_real(whole);
}

// 02 の 14.19: an ordinal as written. 14.8 makes number^ one type of two
// representations, so 3 and 3.0 name the same character -- and a value that
// came out of a division is rounded rather than refused, since a real is the
// ordinary answer there.
//
// The rounding is floor(x + 0.5) and not the round of arithmetic. A negative
// ordinal is resolved by adding an integer, and only a rounding that
// commutes with that gives one answer whichever order the two happen in;
// rounding away from zero does not.
static bool ordinal_of(LhatValue value, int64_t *out)
{
    if (lhat_is_integer(value)) {
        *out = lhat_as_integer(value);
        return true;
    }
    if (!lhat_is_number(value)) {
        return false;
    }
    double real = lhat_number_as_real(value);
    if (!(real > -9.0e15 && real < 9.0e15)) {
        return false;  // past what an ordinal could name either way
    }
    *out = (int64_t)floor(real + 0.5);
    return true;
}

// 14.19: a written ordinal as a position counting from 1. A negative one
// counts from the end, so -1 is the last character.
static int64_t resolve_ordinal(int64_t written, size_t count)
{
    if (written < 0) {
        return (int64_t)count + 1 + written;
    }
    return written;  // 0 stays 0, which no position is, and the caller refuses
}

// 14.9: a table nobody made with a def^. Every name on one is the writer's,
// which is what 14.17改 turns on -- a definition and an instance of it carry
// names 14 章 reserved, and a table literal carries none.
static bool plain_table(LhatValue on)
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
    if (!hatted && plain_table(on)) {
        return false;
    }
    // 02 の 14.22: the table's own operations belong to a plain table alone
    // -- a def^'s names are the writer's, a host type's the library's. The
    // hat rule just above already refused the bare spelling there.
    if (*out >= LHAT_NATIVE_JOIN && *out <= LHAT_NATIVE_CLEAR) {
        return plain_table(on);
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
    if (!lhat_is_nil(found) || plain_table(on)) {
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
static bool fits_call(LhatValue candidate, const LhatValue *at, uint8_t given,
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

// 11.3改: how one side answered the operator it was asked for.
typedef enum {
    OPERATOR_PICKED,       // a candidate was found and takes the other operand
    OPERATOR_ABSENT,       // this side carries no such member
    OPERATOR_NO_CANDIDATE, // it carries a group, and none of it takes this
    OPERATOR_NOT_CALLABLE, // the member is there and is not a subroutine
    OPERATOR_NO_MEMORY
} OperatorLookup;

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
static OperatorLookup operator_candidate(Machine *m, LhatValue side,
                                         const char *name, size_t length,
                                         LhatValue receiver, LhatValue argument,
                                         uint8_t given, bool self_last,
                                         LhatValue *picked)
{
    *picked = lhat_nil();
    const LhatTable *carrier = table_of(side);
    // 05 の 8.9: a host value's operators live in the members table the
    // machine bound for its type -- the value has no heap half of its own.
    if (carrier == NULL && lhat_is_hostvalue(side)) {
        carrier = hostvalue_members_of(m, lhat_as_hostvalue_tag(side));
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
            // two is the same question, which fits_call asks.
            if (!candidate_self_last(group->candidates[i], self_last)) {
                continue;
            }
            size_t skip = 1;
            shaped[0] = group->candidates[i];
            if (fits_call(group->candidates[i], shaped, given, true, &skip)) {
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
        if (!fits_call(found, shaped, given, true, &skip)) {
            return OPERATOR_NO_CANDIDATE;
        }
    }
    *picked = found;
    return OPERATOR_PICKED;
}

// 15.4 with 5.4: the inverse of the move a yield^ makes. The frame is back
// on the stack at `base` -- always as the new top frame, so its slots are
// the highest addresses in use -- and every capture that traveled with the
// saved registers points back into the live slots and rejoins the machine's
// open list at its head. The coroutine's own list is kept in ascending slot
// order, so prepending one by one lands them in the descending order the
// machine's list keeps.
static void reattach_upvalues(Machine *m, LhatCoroutine *co, size_t base)
{
    while (co->open != NULL) {
        LhatUpvalue *up = co->open;
        co->open = up->next_open;
        size_t offset = (size_t)(up->location.value - co->registers.values);
        up->location = lhat_slots_ref(m->slots, base + offset);
        up->suspended_in = NULL;
        up->next_open = m->open;
        m->open = up;
    }
}

// 5.11 with 02 の 10.7: puts a suspended coroutine's one saved frame back on
// the stack so that what it still owes can be run. The caller has already
// made room (LHAT_MAX_FRAMES and the stack's own end) and picked where the
// frame goes: `next_base` is its first register and `result` the slot in the
// caller's frame the answer lands in.
//
// 5.2 fixes a frame's width at compile time and gc.c's mark_roots walks all
// of it -- it has to, since the scratch above the names is where a
// half-finished expression keeps what it is holding. So every slot inside the
// width is read by the collector whether or not this frame ever wrote it, and
// one the frame below left behind still holds that frame's value.
//
// Which is a use-after-free waiting to happen: the value was unreachable when
// its own frame went away and was collected, and the slot still points at it.
// Emptying the scratch here is what keeps the walk seeing nil^ instead. Lua
// clears a new frame's stack for the same reason.
//
// 05 の 8.9: where the parameters end is `parameter_slots` and not the count
// -- a host value parameter is one parameter and several slots. 13.7's
// collector is a parameter of its own and is inside it too, so everything the
// caller laid down is below this and only the scratch is emptied.
static void clear_scratch(Machine *m, size_t base, const LhatProto *proto)
{
    if (proto == NULL) {
        return;
    }
    for (size_t r = proto->parameter_slots; r < proto->chunk.registers; r++) {
        lhat_slots_set(m->slots, base + r, lhat_nil());
    }
}

// The frame comes back marked `disposing`, which 10.7 needs -- a yield^ from
// inside a cleanup has nothing to suspend into -- and set to drain rather
// than to run: every cleanup, innermost first, and then the coroutine is
// finished. What the caller does with `frame`/`registers`/`chunk`/`pc` after
// this is jump to the drain.
static void enter_disposal_frame(Machine *m, LhatCoroutine *co,
                                 size_t next_base, uint8_t result,
                                 Frame **frame, size_t *rbase,
                                 const LhatChunk **chunk, size_t *pc)
{
    for (size_t i = 0; i < co->register_count; i++) {
        lhat_slots_set(m->slots, next_base + i,
                       lhat_slots_get(co->registers, i));
    }
    reattach_upvalues(m, co, next_base);
    Frame *called = &m->frames[m->frame_count++];
    called->closure = co->closure;
    called->pc = co->pc;
    called->base = next_base;
    called->result = result;
    called->prepared = 1;  // 13.8改: a dispose answers nothing to take apart
    called->coroutine = co;
    called->drop_answer = false;  // 5.3
    called->disposing = true;
    called->returning = true;
    called->drain_target = 0;
    called->answer = lhat_nil();
    called->cleanup_count = co->cleanup_count;
    for (size_t i = 0; i < co->cleanup_count; i++) {
        called->cleanups[i] = co->cleanups[i];
    }
    co->state = LHAT_COROUTINE_RUNNING;

    *frame = called;
    *rbase = called->base;
    *chunk = &co->closure->proto->chunk;
    *pc = called->pc;
}

// 5.4: one place per slot, so two closures capturing the same name share it.
// The machine's list only ever holds captures of live stack slots, so the
// payload pointer alone orders it -- the tag pointer travels alongside.
static LhatUpvalue *capture(Machine *m, size_t slot)
{
    const LhatValueUnion *place = m->slots.values + slot;
    LhatUpvalue **link = &m->open;
    while (*link != NULL && (*link)->location.value > place) {
        link = &(*link)->next_open;
    }
    if (*link != NULL && (*link)->location.value == place) {
        return *link;
    }

    LhatUpvalue *upvalue =
        (LhatUpvalue *)lhat_object_alloc(&m->objects, sizeof *upvalue,
                                        LHAT_OBJECT_UPVALUE);
    if (upvalue == NULL) {
        return NULL;
    }
    upvalue->location = lhat_slots_ref(m->slots, slot);
    upvalue->next_open = *link;
    *link = upvalue;
    return upvalue;
}

// 5.12: everything in this file that writes into a table goes through here.
// A table is written over and over, so the barrier it takes is the backward
// one -- the table goes back on the collector's list, to be looked at once
// the writing has settled, rather than every value being marked as it
// arrives. Both the key and the value are stored, so both are asked about.
//
// One place for the barrier to be missing from rather than nine. A table
// made in this same instruction is white and the barrier does nothing, so
// there is no call site that has to know which kind it has.
static bool set_key(Machine *m, LhatTable *table, LhatValue key,
                    LhatValue value, bool *refused)
{
    if (!lhat_table_set(table, key, value, refused)) {
        return false;
    }
    lhat_gc_barrier_back(m, (LhatObject *)table, key);
    lhat_gc_barrier_back(m, (LhatObject *)table, value);
    return true;
}

// 02 の 14.11: the leaves that may sit in a definition's prototype as they
// are. Immutable values -- a subroutine's captured places are its own
// affair, an error kind and a type are identities -- which sharing cannot
// betray. A table is not one: bake_default below copies it into the
// prototype's own sealed tree, and a definition among the values stays
// shared on purpose (a public identity, not per-instance data).
static bool immutable_default(LhatValue value)
{
    if (!lhat_is_object(value)) {
        // nil^, bool^, number^. A host value is bytes on the stack and
        // never a table's to hold, but the answer here is no either way.
        return !lhat_is_hostvalue(value) && value.tag != LHAT_VALUE_CONT &&
               value.tag != LHAT_VALUE_RUN;
    }
    switch (lhat_as_object(value)->kind) {
        case LHAT_OBJECT_STRING:
        case LHAT_OBJECT_SUBROUTINE:
        case LHAT_OBJECT_HOST:
        case LHAT_OBJECT_NATIVE:
        case LHAT_OBJECT_OVERLOAD:
        case LHAT_OBJECT_TYPE:
        case LHAT_OBJECT_ERROR_KIND:
            return true;
        default:
            return false;
    }
}

// 02 の 14.11: a field's value on its way onto the prototype (SETPROTO).
// A table that is not a definition becomes the prototype's own: a sealed
// structural copy, so what every instance starts from belongs to the
// definition alone, whatever expression the initialiser was. Leaves pass
// through; a value nothing may share is refused. The depth cap is the
// C-stack guard config.h describes -- a cycle cannot be a literal tree, so
// it lands here and is refused rather than followed.
static bool bake_default(Machine *m, LhatValue held, LhatValue *out,
                         size_t depth, bool *refused_value)
{
    // 05 の 8.9: a box is a copyable node of depth nought -- bytes, no
    // references -- so the prototype takes a sealed copy of its own.
    if (lhat_is_object_kind(held, LHAT_OBJECT_HOSTVALUE_BOX)) {
        const LhatHostValueBox *source =
            (const LhatHostValueBox *)lhat_as_object(held);
        const LhatHostValueTag *tag = lhat_hostvalue_box_tag(source);
        LhatHostValueBox *copy = lhat_hostvalue_box_new(&m->objects, tag);
        if (copy == NULL) {
            return false;
        }
        memcpy(copy->run, source->run, tag->width * sizeof(LhatValueUnion));
        copy->sealed = true;
        *out = lhat_object((LhatObject *)copy);
        return true;
    }
    if (lhat_is_object_kind(held, LHAT_OBJECT_TABLE)) {
        const LhatTable *table = (const LhatTable *)lhat_as_object(held);
        if (table->is_definition) {
            *out = held;
            return true;
        }
        if (depth > LHAT_MAX_PROTOTYPE_DEPTH) {
            *refused_value = true;
            return false;
        }
        LhatTable *copy = lhat_table_new(&m->objects);
        if (copy == NULL) {
            return false;
        }
        bool key_refused = false;
        for (size_t i = 0; i < table->array_count; i++) {
            LhatValue baked = lhat_nil();
            if (!bake_default(m, lhat_slots_get(table->array, i), &baked,
                              depth + 1, refused_value) ||
                !set_key(m, copy, lhat_integer((int64_t)i + 1), baked,
                         &key_refused)) {
                return false;
            }
        }
        for (size_t i = 0; i < table->entry_capacity; i++) {
            const LhatTableEntry *entry = &table->entries[i];
            if (lhat_is_nil(entry->key)) {
                continue;  // free, or a tombstone
            }
            LhatValue baked = lhat_nil();
            if (!bake_default(m, entry->value, &baked, depth + 1,
                              refused_value) ||
                !set_key(m, copy, entry->key, baked, &key_refused)) {
                return false;
            }
        }
        copy->sealed = true;
        *out = lhat_object((LhatObject *)copy);
        return true;
    }
    if (!immutable_default(held)) {
        *refused_value = true;
        return false;
    }
    *out = held;
    return true;
}

static LhatTable *clone_table(Machine *m, const LhatTable *source,
                              size_t depth, bool *too_deep);

// A field's value on its way into a copy. A table that is not a definition
// is a tree of the instance's own (14.11's literal trees), so it is copied
// too; everything else -- the immutable leaves, and a definition, which is a
// shared identity -- travels as it is.
static bool clone_default(Machine *m, LhatValue held, LhatValue *out,
                          size_t depth, bool *too_deep)
{
    if (lhat_is_object_kind(held, LHAT_OBJECT_TABLE)) {
        const LhatTable *table = (const LhatTable *)lhat_as_object(held);
        if (!table->is_definition) {
            LhatTable *copy = clone_table(m, table, depth + 1, too_deep);
            if (copy == NULL) {
                return false;
            }
            *out = lhat_object((LhatObject *)copy);
            return true;
        }
    }
    // 05 の 8.9: a box is copied by its bytes, unsealed -- each instance's
    // own to set().
    if (lhat_is_object_kind(held, LHAT_OBJECT_HOSTVALUE_BOX)) {
        const LhatHostValueBox *source =
            (const LhatHostValueBox *)lhat_as_object(held);
        const LhatHostValueTag *tag = lhat_hostvalue_box_tag(source);
        LhatHostValueBox *copy = lhat_hostvalue_box_new(&m->objects, tag);
        if (copy == NULL) {
            return false;
        }
        memcpy(copy->run, source->run, tag->width * sizeof(LhatValueUnion));
        *out = lhat_object((LhatObject *)copy);
        return true;
    }
    *out = held;
    return true;
}

// 02 の 14.11: the copy construction makes -- the table's own two halves,
// value by value, a held table copied as its own tree. The link and the seal
// are the caller's to set; a fresh table has neither. The baking (SETPROTO)
// already refused a cycle, so the depth cap only keeps the recursion off the
// C stack's limits for a table that never went through it. NULL when out of
// memory, or with *too_deep set when the cap refused it.
static LhatTable *clone_table(Machine *m, const LhatTable *source,
                              size_t depth, bool *too_deep)
{
    if (depth > LHAT_MAX_PROTOTYPE_DEPTH) {
        *too_deep = true;
        return NULL;
    }
    LhatTable *clone = lhat_table_new(&m->objects);
    if (clone == NULL) {
        return NULL;
    }
    bool refused = false;
    for (size_t i = 0; i < source->array_count; i++) {
        LhatValue held = lhat_nil();
        if (!clone_default(m, lhat_slots_get(source->array, i), &held, depth,
                           too_deep) ||
            !set_key(m, clone, lhat_integer((int64_t)i + 1), held, &refused)) {
            return NULL;
        }
    }
    for (size_t i = 0; i < source->entry_capacity; i++) {
        const LhatTableEntry *entry = &source->entries[i];
        if (lhat_is_nil(entry->key)) {
            continue;  // free, or a tombstone
        }
        LhatValue held = lhat_nil();
        if (!clone_default(m, entry->value, &held, depth, too_deep) ||
            !set_key(m, clone, entry->key, held, &refused)) {
            return NULL;
        }
    }
    return clone;
}

// 02 の 11.2改: '..' between two plain tables concatenates into a new table.
// The sequence halves go in order -- the left's positions and then the
// right's, renumbered after them -- and the named keys come from both sides.
// The copy is shallow: the elements are shared, only the holder is new.
//
// A key both sides carry answers NULL through *collided (14.5改's rule read
// for values: neither side was written against the other, so neither is the
// answer -- and unlike a composed definition's method there is no qualified
// spelling left to reach one through, so it is refused outright).
static LhatTable *concat_tables(Machine *m, const LhatTable *left,
                                const LhatTable *right, bool *collided)
{
    *collided = false;
    LhatTable *joined = lhat_table_new(&m->objects);
    if (joined == NULL) {
        return NULL;
    }
    bool refused = false;
    size_t position = 0;
    const LhatTable *sides[2] = { left, right };
    for (size_t s = 0; s < 2; s++) {
        for (size_t i = 0; i < sides[s]->array_count; i++) {
            if (!set_key(m, joined, lhat_integer((int64_t)++position),
                         lhat_slots_get(sides[s]->array, i), &refused)) {
                return NULL;
            }
        }
    }
    for (size_t s = 0; s < 2; s++) {
        for (size_t i = 0; i < sides[s]->entry_capacity; i++) {
            const LhatTableEntry *entry = &sides[s]->entries[i];
            if (lhat_is_nil(entry->key)) {
                continue;  // free, or a tombstone
            }
            // An integer key past the dense part stays what it was: the two
            // sequences were renumbered above, and a sparse index was never
            // part of either sequence, so it is carried as a named key is.
            // Asked of both sides, not just the right: a sparse index may
            // land where the renumbering put someone's position, and that
            // is a collision like any other.
            if (!lhat_is_nil(lhat_table_get(joined, entry->key))) {
                *collided = true;
                return NULL;
            }
            if (!set_key(m, joined, entry->key, entry->value, &refused)) {
                return NULL;
            }
        }
    }
    return joined;
}

// ---------------------------------------------------------------------------
// 02 の 14.22: the table's own operations
// ---------------------------------------------------------------------------

// One comparison, three-way. `cmp` is the written comparator or nil^ for the
// built-in ordering -- 11.9's own, numbers and strings.
static LhatRunStatus sort_compare(Machine *m, LhatValue cmp, LhatValue x,
                                  LhatValue y, int *out)
{
    if (lhat_is_nil(cmp)) {
        return three_way(x, y, out) ? LHAT_RUN_OK : LHAT_RUN_TYPE_ERROR;
    }
    LhatValue pair[2];
    pair[0] = x;
    pair[1] = y;
    // Nested on purpose -- vm.h says a call may be made inside a running
    // machine, and the comparator is arbitrary L^ code.
    LhatRunResult ran = lhat_machine_call(m, cmp, pair, 2);
    if (ran.status != LHAT_RUN_OK) {
        return ran.status;
    }
    if (!lhat_is_number(ran.value)) {
        return LHAT_RUN_TYPE_ERROR;
    }
    double answer = lhat_number_as_real(ran.value);
    *out = answer < 0 ? -1 : answer > 0 ? 1 : 0;
    return LHAT_RUN_OK;
}

// Merge sort, bottom up, for both spellings -- stable, which sort^ simply
// does not promise. Every pass reads the whole of `from` and writes the
// whole of `into`, so at any moment every element is in one rooted table:
// t is a register's, and the aux rides m->native_hold while the comparator
// (which may allocate and collect) runs. A comparator that faults leaves
// the order unspecified, but every element still present.
static LhatRunStatus table_sort(Machine *m, LhatTable *t, LhatValue cmp)
{
    size_t count = t->array_count;
    if (count < 2) {
        return LHAT_RUN_OK;
    }
    LhatTable *aux = lhat_table_new(&m->objects);
    if (aux == NULL) {
        return LHAT_RUN_OUT_OF_MEMORY;
    }
    bool refused = false;
    for (size_t i = 0; i < count; i++) {
        if (!set_key(m, aux, lhat_integer((int64_t)i + 1),
                     lhat_slots_get(t->array, i), &refused)) {
            return LHAT_RUN_OUT_OF_MEMORY;
        }
    }
    LhatNativeHold hold;
    hold.held = lhat_object((LhatObject *)aux);
    hold.outer = m->native_hold;
    m->native_hold = &hold;
    LhatTable *from = aux;
    LhatTable *into = t;
    LhatRunStatus status = LHAT_RUN_OK;
    for (size_t width = 1; width < count && status == LHAT_RUN_OK;
         width *= 2) {
        for (size_t lo = 0; lo < count && status == LHAT_RUN_OK;
             lo += 2 * width) {
            size_t mid = lo + width < count ? lo + width : count;
            size_t hi = lo + 2 * width < count ? lo + 2 * width : count;
            size_t i = lo;
            size_t j = mid;
            for (size_t k = lo; k < hi; k++) {
                bool take_left;
                if (i >= mid) {
                    take_left = false;
                } else if (j >= hi) {
                    take_left = true;
                } else {
                    int outcome = 0;
                    status = sort_compare(m, cmp,
                                          lhat_slots_get(from->array, i),
                                          lhat_slots_get(from->array, j),
                                          &outcome);
                    if (status != LHAT_RUN_OK) {
                        break;
                    }
                    take_left = outcome <= 0;  // equals keep their order
                }
                LhatValue moved =
                    lhat_slots_get(from->array, take_left ? i++ : j++);
                lhat_slots_set(into->array, k, moved);
                lhat_gc_barrier_back(m, (LhatObject *)into, moved);
            }
        }
        LhatTable *turned = from;
        from = into;
        into = turned;
    }
    // The passes end with the ordered run in `from`.
    if (status == LHAT_RUN_OK && from != t) {
        for (size_t i = 0; i < count; i++) {
            LhatValue moved = lhat_slots_get(from->array, i);
            lhat_slots_set(t->array, i, moved);
            lhat_gc_barrier_back(m, (LhatObject *)t, moved);
        }
    }
    m->native_hold = hold.outer;
    return status;
}

// One string out of the sequence half. Lua's line: a string is itself, a
// number is written the way tostring writes it, anything else is refused.
static LhatRunStatus table_join(Machine *m, const LhatTable *t,
                                const char *sep, size_t sep_length,
                                LhatValue *answer)
{
    size_t count = t->array_count;
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        LhatValue held = lhat_slots_get(t->array, i);
        if (lhat_is_object_kind(held, LHAT_OBJECT_STRING)) {
            total += ((const LhatString *)lhat_as_object(held))->length;
        } else if (lhat_is_number(held)) {
            total += lhat_value_text(held, NULL, 0);
        } else {
            return LHAT_RUN_TYPE_ERROR;
        }
        if (i + 1 < count) {
            total += sep_length;
        }
    }
    char *text = (char *)lhat_alloc(total + 1);
    if (text == NULL) {
        return LHAT_RUN_OUT_OF_MEMORY;
    }
    size_t at = 0;
    for (size_t i = 0; i < count; i++) {
        LhatValue held = lhat_slots_get(t->array, i);
        if (lhat_is_object_kind(held, LHAT_OBJECT_STRING)) {
            const LhatString *piece =
                (const LhatString *)lhat_as_object(held);
            memcpy(text + at, piece->text, piece->length);
            at += piece->length;
        } else {
            at += lhat_value_text(held, text + at, total + 1 - at);
        }
        if (i + 1 < count && sep_length > 0) {
            memcpy(text + at, sep, sep_length);
            at += sep_length;
        }
    }
    LhatString *joined = lhat_string_new(&m->objects, text, at);
    lhat_free(text);
    if (joined == NULL) {
        return LHAT_RUN_OUT_OF_MEMORY;
    }
    *answer = lhat_object((LhatObject *)joined);
    return LHAT_RUN_OK;
}

// The block copy Lua's table.move makes: dst[to .. to+(last-from)] =
// src[from .. last], through get/set so a sparse landing works, backwards
// when the ranges overlap that way round. The caller checked the bounds.
static LhatRunStatus table_blockmove(Machine *m, LhatTable *dst,
                                     const LhatTable *src, int64_t from,
                                     int64_t last, int64_t to)
{
    if (last < from) {
        return LHAT_RUN_OK;  // an empty block moves nothing
    }
    bool refused = false;
    int64_t span = last - from;
    if (dst == src && to > from) {
        for (int64_t k = span; k >= 0; k--) {
            if (!set_key(m, dst, lhat_integer(to + k),
                         lhat_table_get(src, lhat_integer(from + k)),
                         &refused)) {
                return LHAT_RUN_OUT_OF_MEMORY;
            }
        }
    } else {
        for (int64_t k = 0; k <= span; k++) {
            if (!set_key(m, dst, lhat_integer(to + k),
                         lhat_table_get(src, lhat_integer(from + k)),
                         &refused)) {
                return LHAT_RUN_OUT_OF_MEMORY;
            }
        }
    }
    return LHAT_RUN_OK;
}

// ---------------------------------------------------------------------------
// 02 の 14.19改3: the plain string searches
// ---------------------------------------------------------------------------

// The needle's first stand at or after `from`, byte positions both ways.
// A well-formed UTF-8 needle begins with a lead byte and lead bytes never
// continue anything, so a hit always lands on a character boundary.
static bool find_bytes(const char *text, size_t length, size_t from,
                       const char *needle, size_t needle_length, size_t *at)
{
    if (needle_length == 0) {
        *at = from <= length ? from : length;
        return from <= length;
    }
    for (size_t i = from; i + needle_length <= length; i++) {
        if (text[i] == needle[0] &&
            memcmp(text + i, needle, needle_length) == 0) {
            *at = i;
            return true;
        }
    }
    return false;
}

// How many characters begin inside [0, until) -- continuation bytes carry
// no ordinal of their own.
static size_t characters_before(const char *text, size_t until)
{
    size_t count = 0;
    for (size_t i = 0; i < until; i++) {
        if (((unsigned char)text[i] & 0xC0) != 0x80) {
            count++;
        }
    }
    return count;
}

// The walk findall answers: every non-overlapping stand of the needle, as
// 1-based character ordinals. The haystack rides the coroutine's `held`;
// this state owns its copy of the needle.
typedef struct {
    const LhatString *subject;  // kept alive by `held`
    size_t next_byte;
    size_t chars_before;        // characters before next_byte
    size_t needle_length;
    char needle[];
} FindWalk;

static bool findall_step(struct LhatMachine *machine, void *context,
                         const LhatValue *sent, size_t sent_count,
                         LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)sent;
    (void)sent_count;
    FindWalk *walk = (FindWalk *)context;
    if (walk->needle_length == 0) {
        return false;  // nowhere to stand; an empty needle walks nothing
    }
    size_t at = 0;
    if (!find_bytes(walk->subject->text, walk->subject->length,
                    walk->next_byte, walk->needle, walk->needle_length,
                    &at)) {
        return false;
    }
    size_t ordinal = walk->chars_before +
                     characters_before(walk->subject->text + walk->next_byte,
                                       at - walk->next_byte) +
                     1;
    // The next search starts past this stand, and the ordinal count moves
    // with it -- counted over the span walked, never from the top again.
    size_t past = at + walk->needle_length;
    walk->chars_before =
        ordinal - 1 + characters_before(walk->subject->text + at, past - at);
    walk->next_byte = past;
    answers[0] = lhat_integer((int64_t)ordinal);
    *answer_count = 1;
    return true;
}

static void findall_release(struct LhatMachine *machine, void *context,
                            const LhatValue *arguments, size_t count,
                            LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)arguments;
    (void)count;
    (void)answers;
    (void)answer_count;
    lhat_free(context);
}

// 14.22: one value through the clone's written policy -- arbitrary L^ code,
// nested the way a sort's comparator is.
static LhatRunStatus clone_policy(Machine *m, LhatValue policy,
                                  LhatValue held, LhatValue *out)
{
    LhatRunResult ran = lhat_machine_call(m, policy, &held, 1);
    if (ran.status != LHAT_RUN_OK) {
        return ran.status;
    }
    *out = ran.value;
    return LHAT_RUN_OK;
}

// 14.22: the shallow copy, or each value through the policy. The copy under
// construction rides m->native_hold while the policy runs, the way a sort's
// aux table does -- the policy may allocate and collect. The keys are
// carried as they are; only the values are asked about.
static LhatRunStatus table_clone(Machine *m, const LhatTable *t,
                                 LhatValue policy, LhatValue *answer)
{
    LhatTable *copy = lhat_table_new(&m->objects);
    if (copy == NULL) {
        return LHAT_RUN_OUT_OF_MEMORY;
    }
    LhatNativeHold hold;
    hold.held = lhat_object((LhatObject *)copy);
    hold.outer = m->native_hold;
    m->native_hold = &hold;
    LhatRunStatus status = LHAT_RUN_OK;
    bool refused = false;
    for (size_t i = 0; i < t->array_count && status == LHAT_RUN_OK; i++) {
        LhatValue held = lhat_slots_get(t->array, i);
        if (!lhat_is_nil(policy)) {
            status = clone_policy(m, policy, held, &held);
        }
        if (status == LHAT_RUN_OK &&
            !set_key(m, copy, lhat_integer((int64_t)i + 1), held, &refused)) {
            status = LHAT_RUN_OUT_OF_MEMORY;
        }
    }
    for (size_t i = 0; i < t->entry_capacity && status == LHAT_RUN_OK; i++) {
        const LhatTableEntry *entry = &t->entries[i];
        if (lhat_is_nil(entry->key)) {
            continue;  // free, or a tombstone
        }
        LhatValue held = entry->value;
        if (!lhat_is_nil(policy)) {
            status = clone_policy(m, policy, held, &held);
        }
        if (status == LHAT_RUN_OK &&
            !set_key(m, copy, entry->key, held, &refused)) {
            status = LHAT_RUN_OUT_OF_MEMORY;
        }
    }
    m->native_hold = hold.outer;
    if (status == LHAT_RUN_OK) {
        *answer = lhat_object((LhatObject *)copy);
    }
    return status;
}

// The one door for all of 14.22. `args` were copied out of the caller's
// window, so a nested comparator call cannot disturb them; what lands over
// the call goes through *answer.
static LhatRunStatus table_native(Machine *m, const LhatNative *native,
                                  const LhatValue *args, size_t count,
                                  LhatValue *answer)
{
    LhatTable *t = (LhatTable *)lhat_as_object(native->bound);
    size_t n = t->array_count;
    bool refused = false;
    // The mutating half answer the receiver, for chaining; the readers
    // write their own answer over this.
    *answer = native->bound;

    // 05 の 8.6: the machine's own tables are read-only, natives included.
    bool mutates = native->kind == LHAT_NATIVE_INSERT ||
                   native->kind == LHAT_NATIVE_PUSH ||
                   native->kind == LHAT_NATIVE_EXTEND ||
                   native->kind == LHAT_NATIVE_REMOVE ||
                   native->kind == LHAT_NATIVE_POP ||
                   native->kind == LHAT_NATIVE_SORT ||
                   native->kind == LHAT_NATIVE_STABLESORT ||
                   native->kind == LHAT_NATIVE_MOVE ||
                   native->kind == LHAT_NATIVE_REVERSE ||
                   native->kind == LHAT_NATIVE_CLEAR;
    if (mutates && t->sealed) {
        return LHAT_RUN_SEALED;
    }

    switch (native->kind) {
        case LHAT_NATIVE_JOIN: {
            if (count > 1) {
                return LHAT_RUN_ARITY;
            }
            const char *sep = "";
            size_t sep_length = 0;
            if (count == 1) {
                if (!lhat_is_object_kind(args[0], LHAT_OBJECT_STRING)) {
                    return LHAT_RUN_TYPE_ERROR;
                }
                const LhatString *written =
                    (const LhatString *)lhat_as_object(args[0]);
                sep = written->text;
                sep_length = written->length;
            }
            return table_join(m, t, sep, sep_length, answer);
        }

        case LHAT_NATIVE_INDEXOF:
        case LHAT_NATIVE_CONTAINS: {
            if (count != 1) {
                return LHAT_RUN_ARITY;
            }
            bool asking = native->kind == LHAT_NATIVE_CONTAINS;
            *answer = asking ? lhat_bool(false) : lhat_nil();
            for (size_t i = 0; i < n; i++) {
                if (lhat_value_equal(args[0], lhat_slots_get(t->array, i))) {
                    *answer = asking ? lhat_bool(true)
                                     : lhat_integer((int64_t)i + 1);
                    break;
                }
            }
            return LHAT_RUN_OK;
        }

        case LHAT_NATIVE_SLICE: {
            if (count < 1 || count > 2) {
                return LHAT_RUN_ARITY;
            }
            int64_t from = 0;
            int64_t to = 0;
            if (!ordinal_of(args[0], &from) ||
                (count == 2 && !ordinal_of(args[1], &to))) {
                return LHAT_RUN_TYPE_ERROR;
            }
            // 14.19's reading: one ordinal runs to the end, a negative one
            // counts from it, and a range that does not stand answers empty.
            int64_t start = resolve_ordinal(from, n);
            int64_t end = resolve_ordinal(count == 2 ? to : (int64_t)n, n);
            LhatTable *cut = lhat_table_new(&m->objects);
            if (cut == NULL) {
                return LHAT_RUN_OUT_OF_MEMORY;
            }
            if (start >= 1 && start <= end && end <= (int64_t)n) {
                for (int64_t k = start; k <= end; k++) {
                    if (!set_key(m, cut, lhat_integer(k - start + 1),
                                 lhat_slots_get(t->array, (size_t)k - 1),
                                 &refused)) {
                        return LHAT_RUN_OUT_OF_MEMORY;
                    }
                }
            }
            *answer = lhat_object((LhatObject *)cut);
            return LHAT_RUN_OK;
        }

        case LHAT_NATIVE_INSERT: {
            if (count != 2) {
                return LHAT_RUN_ARITY;
            }
            int64_t at_pos = 0;
            if (!ordinal_of(args[0], &at_pos)) {
                return LHAT_RUN_TYPE_ERROR;
            }
            if (at_pos < 1 || at_pos > (int64_t)n + 1) {
                return LHAT_RUN_BAD_KEY;
            }
            for (int64_t k = (int64_t)n; k >= at_pos; k--) {
                if (!set_key(m, t, lhat_integer(k + 1),
                             lhat_slots_get(t->array, (size_t)k - 1),
                             &refused)) {
                    return LHAT_RUN_OUT_OF_MEMORY;
                }
            }
            if (!set_key(m, t, lhat_integer(at_pos), args[1], &refused)) {
                return LHAT_RUN_OUT_OF_MEMORY;
            }
            return LHAT_RUN_OK;
        }

        case LHAT_NATIVE_PUSH: {
            if (count != 1) {
                return LHAT_RUN_ARITY;
            }
            if (!set_key(m, t, lhat_integer((int64_t)n + 1), args[0],
                         &refused)) {
                return LHAT_RUN_OUT_OF_MEMORY;
            }
            return LHAT_RUN_OK;
        }

        case LHAT_NATIVE_EXTEND: {
            if (count != 1) {
                return LHAT_RUN_ARITY;
            }
            if (!lhat_is_object_kind(args[0], LHAT_OBJECT_TABLE)) {
                return LHAT_RUN_TYPE_ERROR;
            }
            const LhatTable *more =
                (const LhatTable *)lhat_as_object(args[0]);
            // The count is read once, so extending a table with itself
            // appends what it held when the call was made.
            size_t held = more->array_count;
            for (size_t i = 0; i < held; i++) {
                if (!set_key(m, t, lhat_integer((int64_t)(n + i) + 1),
                             lhat_slots_get(more->array, i), &refused)) {
                    return LHAT_RUN_OUT_OF_MEMORY;
                }
            }
            return LHAT_RUN_OK;
        }

        case LHAT_NATIVE_REMOVE:
        case LHAT_NATIVE_POP: {
            int64_t at_pos = (int64_t)n;
            if (native->kind == LHAT_NATIVE_REMOVE) {
                if (count != 1) {
                    return LHAT_RUN_ARITY;
                }
                if (!ordinal_of(args[0], &at_pos)) {
                    return LHAT_RUN_TYPE_ERROR;
                }
                at_pos = resolve_ordinal(at_pos, n);
            } else if (count != 0) {
                return LHAT_RUN_ARITY;
            }
            // 04 の 11.3's line: what is not there is not an error. An empty
            // table's pop and an out-of-range remove both answer nil^.
            if (at_pos < 1 || at_pos > (int64_t)n) {
                *answer = lhat_nil();
                return LHAT_RUN_OK;
            }
            *answer = lhat_slots_get(t->array, (size_t)at_pos - 1);
            for (int64_t k = at_pos; k < (int64_t)n; k++) {
                lhat_slots_set(t->array, (size_t)k - 1,
                               lhat_slots_get(t->array, (size_t)k));
            }
            if (!set_key(m, t, lhat_integer((int64_t)n), lhat_nil(),
                         &refused)) {
                return LHAT_RUN_OUT_OF_MEMORY;
            }
            return LHAT_RUN_OK;
        }

        case LHAT_NATIVE_CLONE: {
            if (count > 1) {
                return LHAT_RUN_ARITY;
            }
            return table_clone(m, t, count == 1 ? args[0] : lhat_nil(),
                               answer);
        }

        case LHAT_NATIVE_SORT:
        case LHAT_NATIVE_STABLESORT: {
            if (count > 1) {
                return LHAT_RUN_ARITY;
            }
            return table_sort(m, t, count == 1 ? args[0] : lhat_nil());
        }

        case LHAT_NATIVE_MOVE: {
            bool cross = count >= 1 &&
                         lhat_is_object_kind(args[0], LHAT_OBJECT_TABLE);
            const LhatTable *source =
                cross ? (const LhatTable *)lhat_as_object(args[0]) : t;
            size_t shift = cross ? 1 : 0;
            if (count - shift < 2 || count - shift > 3) {
                return LHAT_RUN_ARITY;
            }
            int64_t from = 0;
            int64_t last = 0;
            int64_t to = 0;
            bool block = count - shift == 3;
            if (!ordinal_of(args[shift], &from) ||
                (block && !ordinal_of(args[shift + 1], &last)) ||
                !ordinal_of(args[count - 1], &to)) {
                return LHAT_RUN_TYPE_ERROR;
            }
            if (!block) {
                last = from;
            }
            if (!cross && !block) {
                // 14.22: the two-ordinal form of self relocates one element,
                // the others shifting to close and open the gap.
                if (from < 1 || from > (int64_t)n || to < 1 ||
                    to > (int64_t)n) {
                    return LHAT_RUN_BAD_KEY;
                }
                LhatValue moved =
                    lhat_slots_get(t->array, (size_t)from - 1);
                if (from < to) {
                    for (int64_t k = from; k < to; k++) {
                        lhat_slots_set(t->array, (size_t)k - 1,
                                       lhat_slots_get(t->array, (size_t)k));
                    }
                } else {
                    for (int64_t k = from; k > to; k--) {
                        lhat_slots_set(
                            t->array, (size_t)k - 1,
                            lhat_slots_get(t->array, (size_t)k - 2));
                    }
                }
                lhat_slots_set(t->array, (size_t)to - 1, moved);
                return LHAT_RUN_OK;
            }
            if (last >= from &&
                (from < 1 || last > (int64_t)source->array_count || to < 1)) {
                return LHAT_RUN_BAD_KEY;
            }
            return table_blockmove(m, t, source, from, last, to);
        }

        case LHAT_NATIVE_REVERSE: {
            if (count != 0) {
                return LHAT_RUN_ARITY;
            }
            for (size_t i = 0; i * 2 + 1 < n; i++) {
                LhatValue held = lhat_slots_get(t->array, i);
                lhat_slots_set(t->array, i,
                               lhat_slots_get(t->array, n - 1 - i));
                lhat_slots_set(t->array, n - 1 - i, held);
            }
            return LHAT_RUN_OK;
        }

        case LHAT_NATIVE_CLEAR: {
            if (count != 0) {
                return LHAT_RUN_ARITY;
            }
            for (size_t i = 0; i < t->array_count; i++) {
                lhat_slots_set(t->array, i, lhat_nil());
            }
            t->array_count = 0;
            for (size_t i = 0; i < t->entry_capacity; i++) {
                t->entries[i].key = lhat_nil();
                t->entries[i].value = lhat_nil();
            }
            t->entry_count = 0;
            return LHAT_RUN_OK;
        }

        default:
            return LHAT_RUN_TYPE_ERROR;  // never reached; the range was asked
    }
}

// Carrying a shared place into the upvalue itself: the value moves into the
// closed cell and location aims there, the same dereference as before.
static void close_into_cell(Machine *m, LhatUpvalue *upvalue)
{
    LhatValue held = lhat_ref_get(upvalue->location);
    upvalue->closed_value = held.as;
    upvalue->closed_tag = (uint8_t)held.tag;
    upvalue->location = lhat_upvalue_closed_ref(upvalue);
    // 5.12: what was a stack slot the roots covered is now a place the
    // upvalue itself holds, so a black upvalue has just gained a reference
    // the collector has not seen.
    lhat_gc_barrier(m, (LhatObject *)upvalue, held);
}

// The frame is going, so anything still pointing into it carries its value
// away. Without this a closure returned from a subroutine would read a slot
// that has been reused.
static void close_upvalues(Machine *m, size_t above)
{
    const LhatValueUnion *floor = m->slots.values + above;
    while (m->open != NULL && m->open->location.value >= floor) {
        LhatUpvalue *upvalue = m->open;
        m->open = upvalue->next_open;
        close_into_cell(m, upvalue);
        upvalue->next_open = NULL;
    }
}

// The same for one place rather than a frame's worth. 03 の 4.3: a session's
// top level puts every let^ of a name back in the one slot, so severing that
// binding's sharing must leave the names above it -- other bindings, still
// live -- shared as they were.
static void close_one_upvalue(Machine *m, size_t slot)
{
    const LhatValueUnion *place = m->slots.values + slot;
    LhatUpvalue **link = &m->open;
    while (*link != NULL) {
        LhatUpvalue *upvalue = *link;
        if (upvalue->location.value != place) {
            link = &upvalue->next_open;
            continue;
        }
        *link = upvalue->next_open;
        close_into_cell(m, upvalue);
        upvalue->next_open = NULL;
    }
}

// What a host function left behind when it returned, read by the three
// places one is called from. 05 の 8.7改: a nested lhat_machine_call that
// faulted leaves its frames standing (nothing unwinds), so frames past
// `frames_before` mean the outer run ends with the same fault. 8.7改2: and
// lhat_machine_panic asks for the same end with a value of the host's own.
// True when the run is over, with what to end it with.
static bool host_faulted(Machine *m, size_t frames_before,
                         LhatRunStatus *status, LhatValue *value)
{
    if (m->host_panicked) {
        m->host_panicked = false;
        *status = LHAT_RUN_PANIC;
        *value = m->fault_value;
        return true;
    }
    if (m->frame_count > frames_before) {
        *status = m->fault_status != LHAT_RUN_OK ? m->fault_status
                                                 : LHAT_RUN_TYPE_ERROR;
        *value = m->fault_value;
        return true;
    }
    return false;
}

// 09 の 2.1: whether the instruction at `at` begins a line the hook should
// be told about, and, when it is, the hook is called here. `frame->pc` is
// already `at + 1`, so lhat_machine_fault_frame reads this instruction.
//
// The rule is lua's: a new line, a jump back onto the same line (a loop), or
// the first instruction of a body just entered. A body is entered or
// returned to whenever the frame count moves, and the line to measure
// against is then the instruction that left -- the CALL, RESUME or YIELD one
// before this frame's, which is `at - 1` in the caller and 0 at a body's
// own top. Returns true when the hook, or a call it made, faulted -- the run
// ends then, the way a host function's fault ends it.
static bool hook_line(Machine *m, Frame *frame, size_t at,
                      LhatRunStatus *status, LhatValue *value)
{
    const LhatChunk *chunk = &frame->closure->proto->chunk;
    size_t old = m->hook_pc;
    if (m->frame_count != m->hook_depth) {
        old = at > 0 ? at - 1 : 0;
        m->hook_depth = m->frame_count;
    }
    m->hook_pc = at;
    // Moving forward within one line is the only case that does not sound:
    // a jump back (at <= old) is a loop, and a changed line is a new line.
    if (at > old && chunk->lines[at] == chunk->lines[old]) {
        return false;
    }
    LhatFrameInfo where;
    lhat_machine_fault_frame((LhatMachine *)m, 0, &where);
    size_t frames_before = m->frame_count;
    m->hook_live = NULL;  // a call the hook makes is not itself hooked
    m->hook((LhatMachine *)m, m->hook_context, LHAT_DEBUG_LINE, &where);
    m->hook_live = m->hook;  // which the hook may have cleared
    return host_faulted(m, frames_before, status, value);
}

static LhatRunResult finish(Machine *m, const LhatChunk *chunk,
                            LhatRunStatus status, LhatValue value, size_t at)
{
    // 03 の 4.3: what the program allocated belongs to the machine, not to
    // the run -- the answer may be a table or a string, and a REPL's next
    // input has to be able to reach it. So a result owns nothing and the
    // answer is good for as long as the machine is.
    LhatRunResult result;
    result.status = status;
    result.value = value;
    // 02 の 13.8改: the positions of a tuple answer, which the pop copied
    // into the machine's room before coming here. Everything else leaves the
    // count at zero, so an ordinary answer reads as it always did.
    result.positions = m->tuple_scratch_count > 0 ? m->tuple_scratch : NULL;
    result.position_count = m->tuple_scratch_count;
    result.at = at;
    // 04 の 11.6改: the frames of this run are still standing (nothing
    // here unwinds), so remember which they were and where the top one
    // stopped -- lhat_machine_fault_* walks them until the next run.
    if (status != LHAT_RUN_OK) {
        m->fault_base = m->run_base;
        m->fault_depth = m->frame_count;
        m->fault_at = at;
        m->fault_status = status;
        m->fault_value = value;
    } else {
        m->fault_depth = m->fault_base = 0;
        m->fault_status = LHAT_RUN_OK;
        m->fault_value = lhat_nil();
    }
    // 04 の 11 章: named from the chunk's own line table (03 の 5.11a-style
    // parallel array) and 02 の 11.8's operator names -- both silently
    // answer nothing usable when `at` is out of range, which only happens
    // for the one fault raised before the first instruction runs.
    result.line = at < chunk->count ? chunk->lines[at] : 0;
    result.op_name =
        at < chunk->count ? operator_name(lhat_op(chunk->code[at]),
                                          &result.op_name_length)
                          : NULL;
    if (result.op_name == NULL) {
        result.op_name_length = 0;
    }
    result.collected = m->collected;
    result.live = m->objects.count;

    // 5.4: an upvalue points into the stack, and the frames are about to go
    // -- but the list is not emptied here. 03 の 4.3: a session's top-level
    // slots outlive the run, and what points into them has to stay listed as
    // open, or the next input could neither find it to close (a let^ writing
    // that name again) nor find it to share (a second closure over the same
    // name). What the run above left pointing into slots that do not survive
    // is closed by the next lhat_run, before it clears them.
    return result;
}

// 02 の 16.3: a table's walk has no body to enter, so one step is the whole
// of resuming it. Both the opcode the loops emit and the start()/resume() a
// program writes by hand take this step -- 16.3 puts a table's iterate() on
// the same footing as any other coroutine, so what the two do has to agree.
typedef enum {
    WALK_TOOK,      // a pair came out; the walk is suspended again
    WALK_ENDED      // nothing left, so the walk is finished
} WalkStep;

// 16.3 with 13.8改: how the walk was asked, which decides where one step's
// tuple goes. The loop says which by RESUME's C -- the count of names is
// syntax, so an unchecked compile says the same thing a checked one does
// (03 の 4.2). No mode allocates.
typedef enum {
    // 'for^ v in^ t': the values of the sequence half, in order, the keyed
    // half not visited -- 'for^ i from^ 1 to^ the length { t[i] }' written
    // as a walk.
    WALK_AS_VALUE,
    // 'for^ k, v in^ t', and a hand-driven call that reserved the width:
    // the pair as a run -- head slot plus two positions in the slots the
    // caller reserved.
    WALK_AS_RUN,
    // 15.8's delegation loop, which reserved one slot: the head goes into
    // it and the positions into the frame's answer room, which is exactly
    // where the YIELD forwarding them reads a tuple's positions from.
    WALK_AS_ANSWER
} WalkMode;

// Puts one step down at slot `at` (WALK_AS_RUN fills at..at+2). The caller
// reserved the slots; the bounds check is the caller's, since the loop's
// reservation and a hand-driven call site reserve different widths.
static WalkStep step_table_walk(Machine *m, LhatCoroutine *co, WalkMode mode,
                                size_t at, Frame *frame)
{
    // 16.3改2: a projection walks the whole table and hands over one half of
    // each step, so the mode says nothing here -- there is one slot to fill
    // whichever way the caller asked. A caller that reserved a run is refused
    // above; this is only ever reached with a slot to write.
    if (co->part != LHAT_WALK_PAIR) {
        LhatValue key, value;
        if (!lhat_table_walk(co, &key, &value)) {
            co->state = LHAT_COROUTINE_DONE;
            lhat_slots_set(m->slots, at, lhat_nil());
            return WALK_ENDED;
        }
        co->state = LHAT_COROUTINE_SUSPENDED;
        lhat_slots_set(m->slots, at,
                       co->part == LHAT_WALK_KEYS ? key : value);
        return WALK_TOOK;
    }

    if (mode == WALK_AS_VALUE) {
        // Only the dense half, by index -- lhat_table_walk would go on into
        // the keyed half, which this form does not visit.
        LhatValue value = lhat_table_get(
            co->walking, lhat_integer((int64_t)co->at_array + 1));
        if (lhat_is_nil(value)) {
            co->state = LHAT_COROUTINE_DONE;
            lhat_slots_set(m->slots, at, lhat_nil());
            return WALK_ENDED;
        }
        co->at_array++;
        co->state = LHAT_COROUTINE_SUSPENDED;
        lhat_slots_set(m->slots, at, value);
        return WALK_TOOK;
    }

    LhatValue key, value;
    if (!lhat_table_walk(co, &key, &value)) {
        co->state = LHAT_COROUTINE_DONE;
        lhat_slots_set(m->slots, at, lhat_nil());
        return WALK_ENDED;
    }
    co->state = LHAT_COROUTINE_SUSPENDED;

    if (mode == WALK_AS_RUN) {
        // 13.8改: the pair as a tuple, laid into the caller's reserved slots.
        lhat_slots_set(m->slots, at, lhat_run_head(2));
        lhat_slots_set(m->slots, at + 1, key);
        lhat_slots_set(m->slots, at + 2, value);
        return WALK_TOOK;
    }

    // WALK_AS_ANSWER: the positions ride the frame's answer room and only
    // the head sits in the slot, which is what the YIELD of 15.8's
    // delegation loop forwards. They stay reachable while they sit there
    // through the table being walked, which the walk holds and the loop
    // holds in a register -- not through the room itself, which gc.c walks
    // only for a frame whose answer is a run.
    frame->answer_run[1] = key.as;
    frame->answer_tags[1] = (uint8_t)key.tag;
    frame->answer_run[2] = value.as;
    frame->answer_tags[2] = (uint8_t)value.tag;
    lhat_slots_set(m->slots, at, lhat_run_head(2));
    return WALK_TOOK;
}

// 05 の 8.8: the same step for a walk the host wrote -- its body is one C
// call rather than a frame, so this is step_table_walk's twin with the value
// already picked by the host. `sent` is what the resume handed in (nil^ from
// the loops, which send nothing). `expected` is the positions a WALK_AS_RUN
// caller reserved, or the slots a WALK_AS_VALUE caller reserved for a wide
// value (0 when it could only be one). A step that answers a shape
// the call site did not reserve puts the mismatch in *fault, LHAT_RUN_OK
// otherwise -- the same refusals a yield^'s placement makes.
// 02 の 13.8改: the boundary's answer, in the shape the machine carries one
// in. Defined below, where the room itself is.
static bool boundary_answer(Machine *m, int written, LhatValue *answered);
static bool call_host_fn(Machine *m, LhatHostFn call, void *context,
                         const LhatValue *arguments, size_t count,
                         LhatValue *answered);

static WalkStep step_host_walk(Machine *m, LhatCoroutine *co, WalkMode mode,
                               size_t at, size_t expected, Frame *frame,
                               const LhatValue *sent, size_t sent_count,
                               LhatRunStatus *fault)
{
    *fault = LHAT_RUN_OK;
    LhatValue out = lhat_nil();
    int written = 0;
    co->state = LHAT_COROUTINE_RUNNING;
    m->tuple_scratch_count = 0;
    bool more = co->step((LhatMachine *)m, co->host_state, sent, sent_count,
                         m->tuple_scratch, &written);
    if (!boundary_answer(m, written, &out)) {
        *fault = LHAT_RUN_TUPLE_ARITY;
        return WALK_ENDED;
    }
    if (!more) {
        // 02 の 10.7: the walk is over, so its state goes back now rather
        // than waiting on a collection.
        co->state = LHAT_COROUTINE_DONE;
        lhat_coroutine_release((LhatObject *)co, m);
        // 13.9: what a walk wrote as it ended is T -- the slot a body
        // coroutine fills by writing return^. It is placed like any
        // other answer, so a hand-written resume() reads it; `for^`
        // does not, because the loop asks ISDONE after the resume and
        // leaves without binding what came back (compile.c).
        if (written == 0) {
            lhat_slots_set(m->slots, at, lhat_nil());
            return WALK_ENDED;
        }
    } else {
        co->state = LHAT_COROUTINE_SUSPENDED;
        if (written == 0) {
            // A walk that goes on has to hand something over: `for^`
            // binds what came back, and there would be nothing to bind.
            *fault = LHAT_RUN_TUPLE_ARITY;
            return WALK_ENDED;
        }
    }

    if (!lhat_is_run(out)) {
        // 05 の 8.9: a host value is written out whole on the spot, exactly
        // as a host call's answer is -- the scratch it lives in belongs to
        // whoever crosses the boundary next, and the next step is exactly
        // such a crossing. Only into room its width fits, whichever mode
        // said it: a run reservation is expected+1 slots, a wide value
        // reservation rides `expected` too, and a site that could not know
        // the type reserved one slot and is refused rather than overwritten
        // past (the delegation loop's slot among them).
        if (lhat_is_hostvalue(out)) {
            const LhatValueUnion *orun = out.as.hostvalue_run;
            size_t room = mode == WALK_AS_RUN ? expected + 1
                          : expected > 0      ? expected
                                              : 1;
            if (orun == NULL || orun[0].hostvalue == NULL ||
                room < orun[0].hostvalue->width ||
                !place_hostvalue_answer(m, at, out)) {
                *fault = LHAT_RUN_TYPE_ERROR;
                return WALK_ENDED;
            }
            return more ? WALK_TOOK : WALK_ENDED;
        }
        if (mode == WALK_AS_RUN) {
            // The loop reserved a run and one value came out.
            *fault = LHAT_RUN_TUPLE_ARITY;
            return WALK_ENDED;
        }
        lhat_slots_set(m->slots, at, out);
        return more ? WALK_TOOK : WALK_ENDED;
    }

    // Several answers: the positions sit in the machine's own room,
    // put there by the host before it returned.
    size_t positions = lhat_run_width(out);
    if (positions != m->tuple_scratch_count) {
        *fault = LHAT_RUN_TUPLE_ARITY;
        return WALK_ENDED;
    }
    if (mode == WALK_AS_VALUE) {
        // One slot reserved and a tuple came out -- 13.8改's refusal.
        *fault = LHAT_RUN_TUPLE_UNEXPECTED;
        return WALK_ENDED;
    }
    if (mode == WALK_AS_RUN) {
        if (positions != expected) {
            *fault = LHAT_RUN_TUPLE_ARITY;
            return WALK_ENDED;
        }
        lhat_slots_set(m->slots, at, out);
        for (size_t i = 0; i < positions; i++) {
            lhat_slots_set(m->slots, at + 1 + i, m->tuple_scratch[i]);
        }
        return more ? WALK_TOOK : WALK_ENDED;
    }

    // WALK_AS_ANSWER, as above: head in the slot, positions in the frame's
    // answer room for the YIELD forwarding them. They stay reachable through
    // the machine's tuple room, whose count stands until the next host
    // boundary -- and the delegation loop reaches its YIELD without one.
    for (size_t i = 0; i < positions; i++) {
        frame->answer_run[i + 1] = m->tuple_scratch[i].as;
        frame->answer_tags[i + 1] = (uint8_t)m->tuple_scratch[i].tag;
    }
    lhat_slots_set(m->slots, at, out);
    return more ? WALK_TOOK : WALK_ENDED;
}

// 5.11: one frame, put back where it left off -- shared by the natives'
// resume, LHAT_BC_RESUME and the host boundary (lhat_machine_resume), which
// all restore a BODY coroutine the same way. `prepared` is what the call
// site reserved for the answer (13.8改) and `result_slot` is where it lands
// in the resumer's window. `sent`/`sent_count` is what the resume sends --
// none, one, or 13.8改's several. The caller has checked the frame and slot
// bounds (a several-send against sent_into included) and saved its own pc.
static Frame *enter_resume_frame(Machine *m, LhatCoroutine *co,
                                 size_t next_base, uint8_t result_slot,
                                 uint8_t prepared, const LhatValue *sent,
                                 size_t sent_count)
{
    bool resuming = co->state == LHAT_COROUTINE_SUSPENDED;
    for (size_t i = 0; i < co->register_count; i++) {
        lhat_slots_set(m->slots, next_base + (i),
                       lhat_slots_get(co->registers, i));
    }
    reattach_upvalues(m, co, next_base);
    Frame *called = &m->frames[m->frame_count++];
    called->closure = co->closure;
    called->pc = co->pc;
    called->base = next_base;
    called->result = result_slot;
    called->prepared = prepared;
    called->coroutine = co;
    called->disposing = false;
    called->drop_answer = false;  // 5.3
    // The room is a root while the frame lives (mark_roots), so it starts
    // empty rather than as whatever the slot held before.
    called->answer = lhat_nil();
    called->derive = LHAT_FRAME_NO_DERIVE;
    called->derive_equal = false;
    called->returning = false;
    called->cleanup_count = co->cleanup_count;
    for (size_t i = 0; i < co->cleanup_count; i++) {
        called->cleanups[i] = co->cleanups[i];
    }
    co->state = LHAT_COROUTINE_RUNNING;
    if (resuming) {
        // 15.4: what the resume sent arrives where the yield^ put out what
        // it sent. 13.8改: several arrive as a run -- head in that slot and
        // the positions after it, which the yield^'s own binding reserved.
        if (sent_count > 1) {
            lhat_slots_set(m->slots, next_base + co->sent_into,
                           lhat_run_head(sent_count));
            for (size_t i = 0; i < sent_count; i++) {
                lhat_slots_set(m->slots, next_base + co->sent_into + 1 + i,
                               sent[i]);
            }
        } else if (sent_count == 1 && lhat_is_hostvalue(sent[0])) {
            // 05 の 8.9: the pointer form the gather built, written out
            // whole where the yield^ receives -- the callers checked the
            // width against the frame already.
            place_hostvalue_answer(m, next_base + co->sent_into, sent[0]);
        } else {
            lhat_slots_set(m->slots, next_base + co->sent_into,
                           sent_count == 1 ? sent[0] : lhat_nil());
        }
    }
    return called;
}

// 05 の 8.6: L^ is the one name that is there without being imported, so what
// it answers is made with the machine. A member is added here and its type in
// check.c's environment_type -- the two lists have to say the same thing.
static bool set_member(Machine *m, LhatTable *table, const char *name,
                       LhatValue value)
{
    LhatString *key = lhat_string_new(&m->objects, name, strlen(name));
    if (key == NULL) {
        return false;
    }
    bool refused = false;
    return set_key(m, table, lhat_object((LhatObject *)key), value, &refused) &&
           !refused;
}

static bool build_environment(Machine *m)
{
    m->environment = lhat_table_new(&m->objects);
    // 05 の 5.3: the registry a unit is loaded into once. Empty until
    // something is loaded, and grown the way 8.8 grows any table.
    LhatTable *modules_value = lhat_table_new(&m->objects);
    LhatNative *collectgarbage_value =
        lhat_native_new(&m->objects, LHAT_NATIVE_COLLECTGARBAGE, lhat_nil());
    if (m->environment == NULL || modules_value == NULL ||
        collectgarbage_value == NULL) {
        return false;
    }
    // environment.h's one list -- check.c's environment_type expands the
    // same one for the types.
#define LHAT_ENVIRONMENT_SET(name, value, type)               \
    if (!set_member(m, m->environment, #name, (value))) {     \
        return false;                                         \
    }
    LHAT_ENVIRONMENT(LHAT_ENVIRONMENT_SET)
#undef LHAT_ENVIRONMENT_SET
    // 05 の 8.6: sealed once built, not before -- the two members above
    // are the machine writing its own table, which is exactly what the mark
    // goes on to refuse from an instruction.
    //
    // The registry inside it is not marked here. 5.3 has a unit register
    // itself, and what it emits for that is an ordinary instruction writing
    // into L^.modules -- the mark would refuse the machine's own bookkeeping.
    // check.c refuses what a writer spells there, which is the half that can
    // be told apart by looking at the source.
    m->environment->sealed = true;
    return true;
}

LhatMachine *lhat_machine_new(void)
{
    // A whole stack and a frame array, so the heap is where it belongs --
    // a static one could serve only one caller and never nest.
    // calloc rather than malloc, and nothing more: 03 の 2.2 numbers
    // LHAT_VALUE_NIL first and gives it a zero payload, so zeroed memory is
    // already a stack full of nil^.
    Machine *m = (Machine *)lhat_calloc(1, sizeof *m);
    if (m == NULL) {
        return NULL;
    }
    // 2.2: the one view every register read goes through, fixed for the
    // machine's whole life. Tag zero is nil^, so the zeroed runs above are
    // already a stack full of it.
    m->slots.values = m->stack_values;
    m->slots.tags = m->stack_tags;
    m->threshold = LHAT_GC_INITIAL_THRESHOLD;
    if (!build_environment(m)) {
        lhat_machine_dispose(m);
        return NULL;
    }
    // 02 の 14.11: the key every construction reads the prototype through.
    m->self_key = lhat_string_new(&m->objects, "self^", 5);
    if (m->self_key == NULL) {
        lhat_machine_dispose(m);
        return NULL;
    }
    // 09 の 5.1: a debugger following every machine hears of this one here,
    // before it has run anything.
    if (lhat_machine_watcher.born != NULL) {
        lhat_machine_watcher.born(lhat_machine_watcher.context,
                                  (LhatMachine *)m);
    }
    return m;
}

bool lhat_machine_make_table(LhatMachine *machine, LhatValue *out)
{
    LhatTable *table = lhat_table_new(&machine->objects);
    if (table == NULL) {
        return false;
    }
    *out = lhat_object((LhatObject *)table);
    return true;
}

bool lhat_machine_table_set(LhatMachine *machine, LhatTable *table,
                            LhatValue key, LhatValue value, bool *refused)
{
    bool ignored = false;
    if (machine == NULL || table == NULL) {
        return false;
    }
    return set_key(machine, table, key, value,
                   refused != NULL ? refused : &ignored);
}

bool lhat_machine_make_host(LhatMachine *machine, LhatHostFn call,
                            void *context, uint8_t parameters,
                            bool has_variadic, bool takes_self, bool self_last,
                            LhatRuntimeType **parameter_types, LhatValue *out)
{
    LhatHost *host = lhat_host_new(&machine->objects, call, context, parameters,
                                   has_variadic, takes_self);
    if (host == NULL) {
        lhat_free(parameter_types);
        return false;
    }
    host->self_last = self_last;
    host->parameter_types = parameter_types;
    *out = lhat_object((LhatObject *)host);
    return true;
}

// 05 の 8.7: the same walk the unit prologue compiles to, done in C because
// nothing is being compiled here. 02 の 8.8's rule holds: a table is made
// where the path does not reach one, and what is there is left alone.
static LhatTable *reach_table(Machine *m, LhatTable *owner, const char *path)
{
    for (const char *segment = path;;) {
        size_t length = strcspn(segment, ".");
        LhatString *key = lhat_string_new(&m->objects, segment, length);
        if (key == NULL) {
            return NULL;
        }
        LhatValue found = lhat_table_get(owner, lhat_object((LhatObject *)key));
        LhatTable *next = table_of(found);
        if (next == NULL) {
            if (!lhat_is_nil(found)) {
                return NULL;  // something that is not a table is there
            }
            next = lhat_table_new(&m->objects);
            bool refused = false;
            if (next == NULL ||
                !set_key(m, owner, lhat_object((LhatObject *)key),
                         lhat_object((LhatObject *)next), &refused) ||
                refused) {
                return NULL;
            }
        }
        owner = next;
        if (segment[length] == '\0') {
            return owner;
        }
        segment += length + 1;
    }
}

bool lhat_machine_make_hostdata(LhatMachine *machine, const LhatHostDataTag *tag,
                            void *pointer, LhatValue *out)
{
    if (machine == NULL || tag == NULL) {
        return false;
    }
    // The members live where the registration put them, which is under the
    // type's own name in L^.modules -- so what answers a call on this value
    // is the same table every other value of the type answers through.
    LhatString *modules_key = lhat_string_new(&machine->objects, "modules", 7);
    if (modules_key == NULL) {
        return false;
    }
    LhatTable *registry = table_of(lhat_table_get(
        machine->environment, lhat_object((LhatObject *)modules_key)));
    if (registry == NULL) {
        return false;
    }
    LhatTable *module = reach_table(machine, registry, tag->module);
    LhatTable *members =
        module != NULL ? reach_table(machine, module, tag->name) : NULL;
    if (members == NULL) {
        return false;
    }

    LhatHostData *data = lhat_hostdata_new(&machine->objects, tag, pointer, members);
    if (data == NULL) {
        return false;
    }
    *out = lhat_object((LhatObject *)data);
    return true;
}

void *lhat_hostdata_pointer(LhatValue value, const LhatHostDataTag *tag)
{
    if (!lhat_is_object_kind(value, LHAT_OBJECT_HOSTDATA)) {
        return NULL;
    }
    const LhatHostData *data = (const LhatHostData *)lhat_as_object(value);
    // 05 の 8.8改: a tag declared under `tag` answers too. That is the
    // promise lhat_register_hostdata_subtype took -- a pointer of the
    // derived type may be read as one of the base's -- and refusing it here
    // would leave the host unable to use the relation it declared.
    for (const LhatHostDataTag *at = data->tag; at != NULL; at = at->base) {
        if (at == tag) {
            return data->pointer;
        }
    }
    return NULL;
}

void *lhat_hostvalue_data(LhatValue argument, const LhatHostValueTag *tag)
{
    if (!lhat_is_hostvalue(argument) || tag == NULL) {
        return NULL;
    }
    const LhatValueUnion *run = argument.as.hostvalue_run;
    if (run == NULL || run[0].hostvalue != tag) {
        return NULL;
    }
    // The head carries the tag; the bytes start one slot later. The cast
    // drops const on purpose: the run is a scratch copy of the caller's,
    // and 8.9 lets a host read or write it freely for the call's duration.
    return (void *)(run + 1);
}

bool lhat_make_hostvalue(LhatMachine *machine, const LhatHostValueTag *tag,
                         const void *bytes, LhatValue *out)
{
    if (machine == NULL || tag == NULL || bytes == NULL || out == NULL ||
        tag->size > LHAT_HOSTVALUE_MAX_BYTES) {
        return false;
    }
    LhatValueUnion *run = machine->hostvalue_scratch;
    // The data run's tail is zeroed so equality can compare whole slots.
    memset(run, 0, tag->width * sizeof *run);
    run[0].hostvalue = tag;
    memcpy(run + 1, bytes, tag->size);
    out->tag = LHAT_VALUE_HOSTVALUE;
    out->as.hostvalue_run = run;
    return true;
}

// 02 の 13.8改 with 05 の 8.7: the boundary's answer, put into the shape the
// machine already carries one in. A host writes its values into the room
// and says how many; from here on a single value travels as itself and
// several travel as a run head with the positions in tuple_scratch --
// which is what every site downstream already reads.
//
// Answers false when the count is out of range, which is a host saying
// something the machine has no room for. The room is LHAT_MAX_TUPLE wide
// and a signature wider than that is refused at registration, so a host
// writing what its own signature says never lands here.
static bool boundary_answer(Machine *m, int written, LhatValue *answered)
{
    if (written < 0 || written > LHAT_MAX_TUPLE) {
        return false;
    }
    if (written == 0) {
        m->tuple_scratch_count = 0;
        *answered = lhat_nil();
        return true;
    }
    if (written == 1) {
        m->tuple_scratch_count = 0;
        *answered = m->tuple_scratch[0];
        return true;
    }
    // Held until the call returns and the machine lays the run down. The
    // count is what tells the collector how much of the room to read.
    m->tuple_scratch_count = (size_t)written;
    *answered = lhat_run_head((size_t)written);
    return true;
}

// One crossing: the arguments over, the answers back. The room is the
// machine's tuple_scratch, which the collector walks -- so a host may
// build a table or call back into L^ between writing an answer and
// returning, and what it wrote stays reachable.
static bool call_host_fn(Machine *m, LhatHostFn call, void *context,
                         const LhatValue *arguments, size_t count,
                         LhatValue *answered)
{
    int written = 0;
    m->tuple_scratch_count = 0;
    call((LhatMachine *)m, context, arguments, count, m->tuple_scratch,
         &written);
    return boundary_answer(m, written, answered);
}

bool lhat_machine_bind_hostvalues(LhatMachine *machine,
                                  const LhatHostValueTypeEntry *entries,
                                  size_t count, size_t slots)
{
    if (machine == NULL || entries == NULL || count == 0 || slots == 0) {
        return false;
    }
    // 05 の 8.9: a tag's index is the process's and not this program's
    // (registry.h), so a program declaring only some of the host value types
    // leaves gaps. The array is taken to the width the indices reach rather
    // than to how many this program declared, and a gap stays NULL -- no
    // value of a type this program never declared can reach a machine of it.
    LhatTable **tables = (LhatTable **)lhat_calloc(slots, sizeof *tables);
    if (tables == NULL) {
        return false;
    }
    LhatString *modules_key = lhat_string_new(&machine->objects, "modules", 7);
    LhatTable *registry =
        modules_key != NULL
            ? table_of(lhat_table_get(machine->environment,
                                      lhat_object((LhatObject *)modules_key)))
            : NULL;
    if (registry == NULL) {
        lhat_free(tables);
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        // The same walk lhat_machine_make_hostdata does for a hostdata
        // type's members: the table registration put under the type's own
        // name is the type's members table.
        LhatTable *module = reach_table(machine, registry, entries[i].tag->module);
        LhatTable *members =
            module != NULL ? reach_table(machine, module, entries[i].tag->name)
                           : NULL;
        if (members == NULL || entries[i].tag->index >= slots) {
            lhat_free(tables);
            return false;
        }
        tables[entries[i].tag->index] = members;
    }
    lhat_free(machine->hostvalue_members);  // a second install replaces
    machine->hostvalue_members = tables;
    machine->hostvalue_member_count = slots;
    return true;
}

// 05 の 5.7: the read-only half of reach_table -- walk to what is there and
// answer NULL where nothing is, making nothing on the way. A path that does
// not reach a table is a path with nothing to forget.
static LhatTable *table_at(Machine *m, LhatTable *owner, const char *segment,
                           size_t length)
{
    LhatString *key = lhat_string_new(&m->objects, segment, length);
    if (key == NULL) {
        return NULL;
    }
    return table_of(lhat_table_get(owner, lhat_object((LhatObject *)key)));
}

bool lhat_machine_forget_unit(LhatMachine *machine, const char *module)
{
    if (machine == NULL || machine->environment == NULL || module == NULL ||
        *module == '\0') {
        return false;
    }
    LhatString *modules_key = lhat_string_new(&machine->objects, "modules", 7);
    if (modules_key == NULL) {
        return false;
    }
    LhatTable *owner = table_of(lhat_table_get(
        machine->environment, lhat_object((LhatObject *)modules_key)));
    if (owner == NULL) {
        return false;
    }

    // Down to the table the last segment sits in. The ones above it stay:
    // other modules live under them, and 5.3's guard tests each step for
    // nil^ anyway, so an empty table left behind means the same as no table.
    const char *segment = module;
    size_t length = strcspn(segment, ".");
    while (segment[length] == '.') {
        owner = table_at(machine, owner, segment, length);
        if (owner == NULL) {
            return false;
        }
        segment += length + 1;
        length = strcspn(segment, ".");
    }

    LhatString *last = lhat_string_new(&machine->objects, segment, length);
    if (last == NULL) {
        return false;
    }
    LhatValue key = lhat_object((LhatObject *)last);
    if (lhat_is_nil(lhat_table_get(owner, key))) {
        return false;  // nothing stood there
    }
    // 04 の 11.3 spells "not there" nil^, which is exactly what 5.3's guard
    // tests -- so storing nil^ is the whole of forgetting.
    bool refused = false;
    return set_key(machine, owner, key, lhat_nil(), &refused) && !refused;
}

bool lhat_machine_register(LhatMachine *machine, const char *module,
                           const char *type, const char *name, LhatValue value)
{
    if (machine == NULL || machine->environment == NULL) {
        return false;
    }
    LhatString *modules_key = lhat_string_new(&machine->objects, "modules", 7);
    if (modules_key == NULL) {
        return false;
    }
    LhatTable *owner = table_of(lhat_table_get(
        machine->environment, lhat_object((LhatObject *)modules_key)));
    if (owner == NULL) {
        return false;
    }

    owner = reach_table(machine, owner, module);
    if (owner != NULL && type != NULL) {
        owner = reach_table(machine, owner, type);
    }
    if (owner == NULL) {
        return false;
    }

    LhatString *key = lhat_string_new(&machine->objects, name, strlen(name));
    if (key == NULL) {
        return false;
    }
    // 02 の 14.12: a second registration of one name is another arm, not a
    // replacement -- the same thing OVERLOADINDEX makes of a member written
    // twice, built here because a registration compiles to no instruction.
    // 05 の 8.7 has already refused any pair that could take the same call.
    LhatValue held = lhat_table_get(owner, lhat_object((LhatObject *)key));
    if (!lhat_is_nil(held)) {
        LhatOverload *group = NULL;
        if (lhat_is_object_kind(held, LHAT_OBJECT_OVERLOAD)) {
            group = (LhatOverload *)lhat_as_object(held);
        } else {
            group = lhat_overload_new(&machine->objects);
            if (group == NULL || !lhat_overload_add(group, held)) {
                return false;
            }
        }
        if (!lhat_overload_add(group, value)) {
            return false;
        }
        value = lhat_object((LhatObject *)group);
    }
    bool refused = false;
    return set_key(machine, owner, lhat_object((LhatObject *)key), value,
                   &refused) && !refused;
}

// 05 の 8.8改: puts a registered type's members table under its base's, so
// that a member the base declared is found by WALKING rather than by having
// been copied down when the program was installed. Copying meant a pass over
// every registration for every type and every one of its ancestors -- a
// binding with a class per engine class waited seconds for it -- and it was
// a second copy of what the checker had already stopped making (the base
// link on the type, type.h).
//
// `definition` is the link table_get_in already climbs, and `is_definition`
// is what tells it this climb is 14.5's walk up to a base and passes
// everything, rather than 14.7's walk from an instance which lets only what
// takes a receiver through. Nearest wins by the order of the walk, which is
// what the copy had to work out by hand.
bool lhat_machine_link_hostdata_base(LhatMachine *machine,
                                     const char *module, const char *name,
                                     const char *base_module,
                                     const char *base_name)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || m->environment == NULL) {
        return false;
    }
    LhatString *modules_key = lhat_string_new(&m->objects, "modules", 7);
    if (modules_key == NULL) {
        return false;
    }
    LhatTable *root = table_of(
        lhat_table_get(m->environment, lhat_object((LhatObject *)modules_key)));
    if (root == NULL) {
        return false;
    }
    LhatTable *owner = reach_table(m, root, module);
    LhatTable *derived = owner != NULL ? reach_table(m, owner, name) : NULL;
    LhatTable *base_owner = reach_table(m, root, base_module);
    LhatTable *base =
        base_owner != NULL ? reach_table(m, base_owner, base_name) : NULL;
    if (derived == NULL || base == NULL || derived == base) {
        return false;
    }
    derived->definition = base;
    derived->is_definition = true;
    base->is_definition = true;
    return true;
}

bool lhat_machine_set_global(LhatMachine *machine, const char *name,
                             LhatValue value)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || m->environment == NULL || name == NULL) {
        return false;
    }
    // 05 の 8.6: set_key rather than an instruction, so the seal on L^ is
    // not in the way -- what it refuses is what a program writes, and this is
    // the host writing what the program will read.
    return set_member(m, m->environment, name, value);
}

void lhat_machine_dispose(LhatMachine *machine)
{
    if (machine == NULL) {
        return;
    }
    // 09 の 5.1: before anything else -- a watcher takes its hook off here,
    // and whatever the disposal below still runs must not sound it.
    if (lhat_machine_watcher.dying != NULL) {
        lhat_machine_watcher.dying(lhat_machine_watcher.context, machine);
    }
    // 05 の 8.8: what the host made goes back before anything is freed, so a
    // release may still read the value it is given. Reachability is not asked
    // -- the machine is going, so everything on it is.
    for (LhatObject *object = machine->objects.objects; object != NULL;
         object = object->next) {
        lhat_hostdata_release(object, machine);
        lhat_coroutine_release(object, machine);
    }
    lhat_object_free_all(&machine->objects);
    // 05 の 8.9: the tables were the heap's (freed just above); the array
    // alone is the machine's.
    lhat_free(machine->hostvalue_members);
    lhat_free(machine);
}

// The run loop itself, shared by lhat_run (base_depth == 0, a fresh unit
// entered through its own wrapper closure) and lhat_machine_call
// (base_depth == m->frame_count at the time of the call, a value already
// callable pushed as one more frame). "the run is over" means the frame
// count has drained back down to base_depth, not to zero -- 0 stays right
// for lhat_run because nothing was on the machine before it pushed frame 0.
// 2.2: the frame's registers, read and written through the machine's two
// parallel runs. `m` and `rbase` are run_frames' own locals; nothing outside
// it may use these.
#define R(i) lhat_slots_get(m->slots, rbase + (size_t)(i))
#define SET_R(i, v) lhat_slots_set(m->slots, rbase + (size_t)(i), (v))

// `draining` enters at the drain rather than at the frame's pc: the frame on
// top is a disposal one (enter_disposal_frame), which has no instructions of
// its own to run -- only cleanups to walk. It is what the loop's own
// `goto drain` does for a disposal it entered itself, said from outside so
// that a host can start one (lhat_machine_collectgarbage).
static LhatRunResult run_frames_loop(Machine *m, size_t base_depth,
                                     bool draining);

// 04 の 11.6改: a nested run -- a host calling back in -- borrows run_base
// for as long as it runs and hands it back, so a fault in the run it came
// out of still bounds that run's own frames rather than the nested one's.
static LhatRunResult run_frames(Machine *m, size_t base_depth, bool draining)
{
    size_t outer = m->run_base;
    LhatRunResult result = run_frames_loop(m, base_depth, draining);
    m->run_base = outer;
    return result;
}

static LhatRunResult run_frames_loop(Machine *m, size_t base_depth,
                                     bool draining)
{
    m->run_base = base_depth;  // 04 の 11.6改: so finish can bound a fault
    Frame *frame = &m->frames[m->frame_count - 1];
    const LhatChunk *chunk = &frame->closure->proto->chunk;
    size_t rbase = frame->base;
    size_t pc = frame->pc;

    // 02 の 10.7: the last collection of the run, and what it found. See the
    // drain, which is where both are decided. `at` is hoisted out of the
    // loop only so that `ending` can jump to the drain without stepping over
    // its initialiser -- which is what lets `draining` jump there too.
    bool end_swept = false;
    bool ending = false;
    size_t at = 0;

    if (draining) {
        goto drain;
    }

    while (pc < chunk->count) {
        // Between instructions, where every live value is in a register, a
        // frame or the open list. Inside one there is a half-built object the
        // roots do not name yet -- which is also why a write made in the same
        // instruction that made the object it goes into needs no barrier.
        //
        // 5.12: a step, not a collection. What this costs is bounded by
        // LHAT_GC_STEP_WORK whatever the heap has grown to.
        if (m->objects.count >= m->threshold) {
            frame->pc = pc;
            lhat_gc_step(m);
        }

        // 02 の 10.7: and here is where what the collector held back gets to
        // run. The heap is whole again and this is an ordinary place to call
        // L^ code, which is exactly what a finally^ body is. One coroutine
        // per boundary -- the rest keep, so a long list never becomes one
        // long pause.
        //
        // Never on top of a disposal already under way (`disposing`): a
        // cleanup is 10.7's own unwinding and another coroutine's cleanups
        // cutting into it would interleave two unwindings that have nothing
        // to do with each other. The queue waits; it is in no hurry.
        if (m->pending_dispose != NULL && !frame->disposing) {
            if (m->frame_count >= LHAT_MAX_FRAMES) {
                return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), pc);
            }
            // Just past what this frame uses, so nothing live is written
            // over. Disposal answers nil^ and no one is asking, but the
            // return still lands somewhere and this is somewhere harmless.
            uint8_t into = chunk->registers;
            size_t next_base = rbase + (into) + 1;
            if (next_base + LHAT_MAX_REGISTERS >= LHAT_STACK_SLOTS) {
                return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), pc);
            }
            LhatCoroutine *co = lhat_gc_take_pending(m);
            frame->pc = pc;
            enter_disposal_frame(m, co, next_base, into, &frame, &rbase,
                                 &chunk, &pc);
            // 15.4: same as an explicit dispose -- the slot a yield^ answers
            // into gets nil^, since nothing sent anything in.
            SET_R(co->sent_into, lhat_nil());
            goto drain;
        }

        // 02 の 10.7: the run already ended and only the queue was holding
        // it open. It is empty now, so back to the frame that was leaving.
        // Only once that frame is on top again -- while a cleanup of its
        // own is running there is a frame above it with instructions left.
        if (ending && m->frame_count == base_depth + 1) {
            goto drain;
        }

        LhatInstruction instruction = chunk->code[pc];
        at = pc++;

        // 09 の 2 章: the debugger's line hook, at the same boundary as the
        // GC step above -- every live value is in a register, a frame or the
        // open list. `frame->pc` is written first (to `at + 1`), so a
        // traceback the hook reads names this instruction, not the last.
        if (m->hook_live != NULL) {
            frame->pc = pc;
            LhatRunStatus left;
            LhatValue left_with;
            if (hook_line(m, frame, at, &left, &left_with)) {
                return finish(m, chunk, left, left_with, at);
            }
        }

        uint8_t a = lhat_a(instruction);
        uint8_t b = lhat_b(instruction);
        uint8_t cc = lhat_c(instruction);
        LhatOpcode op = lhat_op(instruction);
        // 02 の 11.9: the comparison that sent control to call_operator,
        // when one did. `op` becomes SPACESHIP there -- that is the member to
        // look for -- and this is what the answer gets read against zero with.
        LhatOpcode derive_from = LHAT_FRAME_NO_DERIVE;
        // 03 の 5.1改: what GETINDEX and GETMEMBER share. Declared out here
        // because the second jumps into the first's body having settled them
        // -- the key it asks by, and the cache to fill on the way out (NULL
        // for a GETINDEX, which remembers nothing).
        //
        // Left unwritten on purpose: both cases set both before jumping in,
        // and every other instruction would pay for the stores.
        LhatValue member_key;
        LhatMemberCache *filling;

        switch (op) {
            case LHAT_BC_LOADK:
                SET_R(a, chunk->constants[lhat_bx(instruction)]);
                break;
            case LHAT_BC_LOADNIL:
                SET_R(a, lhat_nil());
                break;
            case LHAT_BC_LOADBOOL:
                SET_R(a, lhat_bool(b != 0));
                break;
            case LHAT_BC_MOVE:
                SET_R(a, R(b));
                break;

            case LHAT_BC_ADD:
            case LHAT_BC_SUB:
            case LHAT_BC_MUL:
            case LHAT_BC_DIV:
            case LHAT_BC_IDIV:
            case LHAT_BC_MOD:
            case LHAT_BC_POW: {
                LhatValue out;
                LhatRunStatus status = LHAT_RUN_OK;
                if (arithmetic(op, R(b), R(cc), &out,
                               &status)) {
                    SET_R(a, out);
                    break;
                }
                // 02 の 11.3: numbers answer built in; anything else answers
                // with the member 11.8 names, or not at all.
                //
                // 11.3改: the right operand is reason enough to go on.
                // '1 + v' is the case the rule exists for -- number^ carries
                // the arithmetic and takes only its own kind, so the answer
                // can only be on the other side.
                // 05 の 8.9: a host value carries its registered operators
                // the same way a table carries 11.8's members.
                if (status != LHAT_RUN_TYPE_ERROR ||
                    (table_of(R(b)) == NULL && table_of(R(cc)) == NULL &&
                     !lhat_is_hostvalue(R(b)) && !lhat_is_hostvalue(R(cc)))) {
                    return finish(m, chunk, status, lhat_nil(), at);
                }
                goto call_operator;
            }

            case LHAT_BC_NEG: {
                // 02 の 11.8改: number^ carries its own negation and pays
                // nothing for the search, the same posture the binary
                // instructions take. Anything else asks for the member.
                if (!lhat_is_number(R(b))) {
                    goto call_operator;
                }
                // 14.8改: INT64_MIN is the one integer whose negation is not
                // an integer, so it widens like any other overflow.
                int64_t negated = 0;
                SET_R(a,
                    lhat_is_integer(R(b)) &&
                            subtract_exact(0, lhat_as_integer(R(b)),
                                           &negated)
                        ? lhat_integer(negated)
                        : lhat_real(-lhat_number_as_real(R(b))));
                break;
            }

            case LHAT_BC_NOT: {
                // 02 の 5.4's condition rule: only a bool is a truth value,
                // so this refuses anything else rather than inventing one.
                if (!lhat_is_bool(R(b))) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                SET_R(a, lhat_bool(!lhat_as_bool(R(b))));
                break;
            }

            case LHAT_BC_TYPEOF: {
                LhatRuntimeType *type = tag_type(&m->objects, R(b));
                if (type == NULL) {
                    return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                SET_R(a, lhat_object((LhatObject *)type));
                break;
            }

            // 11.9 with 11.9改: a type saying what equals what is asked
            // before the default answer is taken. The member is looked for
            // as '=' first and as '<=>' second, and whatever neither answers
            // falls back on what the value was already the same as -- 14.2's
            // identity for a table, 05 の 8.9's bytes for a host value. That
            // is what leaves every other value exactly as it was.
            case LHAT_BC_EQ:
            case LHAT_BC_NE:
                if (table_of(R(b)) != NULL || table_of(R(cc)) != NULL ||
                    lhat_is_hostvalue(R(b)) || lhat_is_hostvalue(R(cc))) {
                    derive_from = op;
                    op = LHAT_BC_EQ;  // the name to look for; '≠' has none
                    goto call_operator;
                }
                // 14.8: two numbers within the error a real carries are one
                // number here. A key, a constant and 'is^' go on asking the
                // exact question -- lhat_value_close is only what '=' and
                // 11.9's orderings read.
                SET_R(a, lhat_bool(
                    lhat_value_close(R(b), R(cc), LHAT_NUMBER_TOLERANCE) ==
                    (op == LHAT_BC_EQ)));
                break;
            case LHAT_BC_SAME:
                // 05 の 8.9: a value type has no identity apart from its
                // bytes, so "the same" is the equality above.
                if (lhat_is_hostvalue(R(b)) || lhat_is_hostvalue(R(cc))) {
                    SET_R(a, lhat_bool(lhat_is_hostvalue(R(b)) &&
                                       hostvalue_equal(m->slots, rbase + b,
                                                       rbase + cc)));
                    break;
                }
                SET_R(a, lhat_bool(
                    lhat_value_same(R(b), R(cc))));
                break;

            case LHAT_BC_LT:
            case LHAT_BC_LE:
            case LHAT_BC_GT:
            case LHAT_BC_GE: {
                bool out = false;
                LhatRunStatus status = LHAT_RUN_OK;
                if (ordering(op, R(b), R(cc), &out, &status)) {
                    SET_R(a, lhat_bool(out));
                    break;
                }
                // 11.9: numbers order themselves; anything else says
                // how it orders with a '<=>', and this reads the answer.
                derive_from = op;
                op = LHAT_BC_SPACESHIP;
                goto call_operator;
            }

            // 11.9: written out. number^ and string^ each order their
            // own; anything else answers with the member 11.8 names.
            case LHAT_BC_SPACESHIP: {
                int outcome = 0;
                if (!three_way(R(b), R(cc), &outcome)) {
                    goto call_operator;
                }
                SET_R(a, lhat_integer(outcome));
                break;
            }

            case LHAT_BC_JUMP:
                pc = (size_t)((int64_t)pc + lhat_jump_offset(instruction));
                break;

            case LHAT_BC_JUMP_FALSE:
                if (!lhat_is_bool(R(a))) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                if (!lhat_as_bool(R(a))) {
                    pc = (size_t)((int64_t)pc + lhat_jump_offset(instruction));
                }
                break;

            case LHAT_BC_CLOSURE: {
                const LhatProto *nested =
                    frame->closure->proto->protos[lhat_bx(instruction)];
                LhatClosure *closure = (LhatClosure *)lhat_object_alloc(
                    &m->objects, sizeof *closure, LHAT_OBJECT_SUBROUTINE);
                if (closure == NULL) {
                    return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                closure->proto = nested;
                closure->upvalue_count = nested->upvalue_count;
                if (nested->upvalue_count > 0) {
                    closure->upvalues = (LhatUpvalue **)lhat_calloc(
                        nested->upvalue_count, sizeof *closure->upvalues);
                    if (closure->upvalues == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                }
                // 5.4: a register of this frame, one of its own upvalues
                // when the name came from further out, or -- 15.10's
                // this^^ -- this frame's own closure, boxed closed on the
                // spot: nothing on the stack holds it, so there is nothing
                // to keep open.
                for (size_t i = 0; i < nested->upvalue_count; i++) {
                    const LhatUpvalueDesc *desc = &nested->upvalues[i];
                    LhatUpvalue *made = NULL;
                    switch (desc->source) {
                        case LHAT_UPVALUE_REGISTER:
                            made = capture(m, rbase + desc->index);
                            break;
                        case LHAT_UPVALUE_OUTER:
                            made = frame->closure->upvalues[desc->index];
                            break;
                        case LHAT_UPVALUE_THIS:
                            made = (LhatUpvalue *)lhat_object_alloc(
                                &m->objects, sizeof *made,
                                LHAT_OBJECT_UPVALUE);
                            if (made != NULL) {
                                lhat_ref_set(
                                    lhat_upvalue_closed_ref(made),
                                    lhat_object(
                                        (LhatObject *)frame->closure));
                                made->location =
                                    lhat_upvalue_closed_ref(made);
                                made->next_open = NULL;
                            }
                            break;
                    }
                    closure->upvalues[i] = made;
                    if (made == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                }
                SET_R(a, lhat_object((LhatObject *)closure));
                break;
            }

            // 03 の 5.11c: strict already found the one arm that fits (14.12
            // leaves at most one), so the call ahead takes it instead of
            // asking every candidate again. Nothing else is touched: a value
            // that is not a group reached here some way the checker did not
            // read, and the ordinary call is still right for it.
            case LHAT_BC_PICKARM: {
                if (lhat_is_object_kind(R(a), LHAT_OBJECT_OVERLOAD)) {
                    const LhatOverload *group =
                        (const LhatOverload *)lhat_as_object(R(a));
                    size_t which = lhat_bx(instruction);
                    if (which < group->count) {
                        SET_R(a, group->candidates[which]);
                    }
                }
                break;
            }

            case LHAT_BC_GETUPVAL:
                SET_R(a, lhat_ref_get(frame->closure->upvalues[b]->location));
                break;

            case LHAT_BC_SETUPVAL: {
                // 5.12: an open upvalue's place is a stack slot and the
                // stack is a root, so only a closed one gains anything the
                // collector has not seen. The barrier asks which it is by
                // asking whether the upvalue is black.
                LhatUpvalue *upvalue = frame->closure->upvalues[b];
                lhat_ref_set(upvalue->location, R(a));
                lhat_gc_barrier(m, (LhatObject *)upvalue, R(a));
                break;
            }

            // 02 の 11.2: '..' is concatenation in general, and strings are
            // the case that is settled. 11.3 leaves the rest to the
            // operator's own definition, which needs op^.
            case LHAT_BC_CONCAT: {
                if (lhat_is_object_kind(R(b), LHAT_OBJECT_STRING) &&
                    lhat_is_object_kind(R(cc), LHAT_OBJECT_STRING)) {
                    const LhatString *left =
                        (const LhatString *)lhat_as_object(R(b));
                    const LhatString *right =
                        (const LhatString *)lhat_as_object(R(cc));
                    LhatString *joined =
                        lhat_string_concat(&m->objects, left, right);
                    if (joined == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                    SET_R(a, lhat_object((LhatObject *)joined));
                    break;
                }

                // 02 の 11.2改: two plain tables concatenate, built in the
                // way two strings do. A definition never comes through here
                // (14.2 settles composition at compile time), and a table
                // that carries an op^.. of its own is not plain -- it is an
                // instance, and falls through to the search below.
                if (plain_table(R(b)) && plain_table(R(cc))) {
                    bool collided = false;
                    LhatTable *joined = concat_tables(
                        m, (const LhatTable *)lhat_as_object(R(b)),
                        (const LhatTable *)lhat_as_object(R(cc)), &collided);
                    if (joined == NULL) {
                        return finish(m, chunk,
                                      collided ? LHAT_RUN_BAD_KEY
                                               : LHAT_RUN_OUT_OF_MEMORY,
                                      lhat_nil(), at);
                    }
                    SET_R(a, lhat_object((LhatObject *)joined));
                    break;
                }

                // 02 の 11.3: a string answers above, built in. Anything else
                // answers with the member 11.8 names, or not at all -- and
                // 11.3改 reads the right operand for one too.
                if (table_of(R(b)) == NULL && table_of(R(cc)) == NULL) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                goto call_operator;
            }

            case LHAT_BC_CLOSE:
                close_upvalues(m, rbase + a);
                break;

            case LHAT_BC_CLOSEONE:
                close_one_upvalue(m, rbase + a);
                break;

            // 02 の 15.10: the frame already holds it, so naming it costs a
            // move rather than a capture.
            case LHAT_BC_THIS:
                SET_R(a,
                    lhat_object((LhatObject *)(void *)frame->closure));
                break;

            // 05 の 8.6: one table per machine, so naming it is a move too.
            case LHAT_BC_ENV:
                SET_R(a, lhat_object((LhatObject *)m->environment));
                break;

            // 05 の 5.3: a unit is a body like any other, so requiring it is
            // making a closure of it and calling that. What makes it load
            // once is the guard the unit itself begins with, not this.
            case LHAT_BC_UNIT: {
                // The number indexes the table of the unit this body was
                // written in (LhatUnitTable), not anything of the machine's
                // -- which is what lets a program grow under it.
                size_t which = lhat_bx(instruction);
                const LhatUnitTable *units = frame->closure->proto->units;
                if (units == NULL || which >= units->count ||
                    units->protos[which] == NULL) {
                    return finish(m, chunk, LHAT_RUN_NO_SUCH_UNIT, lhat_nil(), at);
                }
                LhatClosure *closure = (LhatClosure *)lhat_object_alloc(
                    &m->objects, sizeof *closure, LHAT_OBJECT_SUBROUTINE);
                if (closure == NULL) {
                    return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                closure->proto = units->protos[which];
                closure->upvalues = NULL;
                closure->upvalue_count = 0;
                SET_R(a, lhat_object((LhatObject *)closure));
                break;
            }

            case LHAT_BC_NEWTABLE: {
                LhatTable *table = lhat_table_new(&m->objects);
                if (table == NULL) {
                    return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                table->is_definition = b != 0;  // 14.9
                SET_R(a, lhat_object((LhatObject *)table));
                break;
            }

            // 05 の 8.6: what require^ answers is the machine's record
            // of what a unit published. Sealed once it is built, so that a
            // requiring unit cannot add to it or write over it -- not even by
            // passing it to a p^ whose parameter is written t^{ … }, which is
            // the one path check.c cannot name.
            case LHAT_BC_SEAL: {
                LhatTable *table = table_of(R(a));
                if (table == NULL) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                table->sealed = true;
                break;
            }

            // 04 の 11.3: a key that is not there answers nil^, so the only
            // way this fails is being asked of something that is not a table.
            // An error answers from its fields: 2.3 gives every kind message
            // and cause, and they are reached the same way as a member.
            // 03 の 5.1改: a written member name, which is the same key every
            // time this site runs. What it remembers is where the answer was
            // last found, and a hit is two comparisons -- against a walk of
            // the 14.7 chain and a probe with a full key equality in it.
            //
            // A miss falls into the body below having settled `key` from the
            // cache and `filling` to the cache, so the ordinary read answers
            // and fills it on the way out. That way there is one place that
            // decides what a member read means, and this is only about how
            // long it takes (4.2).
            case LHAT_BC_GETMEMBER: {
                LhatMemberCache *cache = &chunk->member_caches[cc];
                const LhatTable *start = readable_table(R(b));
                if (cache->answered != NULL && start != NULL &&
                    (cache->from_definition
                         // An instance is a fresh table per value, so what is
                         // compared is not the table but the fact that it has
                         // never been structurally written since it was cloned
                         // -- and 5.10 seals the prototype, so such a clone
                         // carries the prototype's keys and no others and
                         // cannot be shadowing this member.
                         ? (start->version == 0 &&
                            start->definition == cache->answered &&
                            cache->answered->version == cache->version)
                         // 05 の 8.8 shares one members table per host type,
                         // so for those this is the same table every time.
                         : (start == cache->answered &&
                            start->version == cache->version))) {
                    SET_R(a, cache->answered->entries[cache->index].value);
                    break;
                }
                member_key = chunk->constants[cache->key];
                filling = cache;
                goto member_body;
            }

            case LHAT_BC_GETINDEX:
                member_key = R(cc);
                filling = NULL;
                goto member_body;

            member_body: {
                // 02 の 12.6 and 15.6: a coroutine answers the operations the
                // runtime provides, bound to what they came through.
                if (lhat_is_object_kind(R(b), LHAT_OBJECT_COROUTINE)) {
                    LhatNativeKind which;
                    bool hatted = false;
                    if (!native_named(member_key, &which, &hatted)) {
                        return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                    }
                    LhatNative *native =
                        lhat_native_new(&m->objects, which, R(b));
                    if (native == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                    SET_R(a, lhat_object((LhatObject *)native));
                    break;
                }
                // 02 の 14.16: a type-info value carries exactly one member.
                // It is not a table, so the ordinary lookup below never sees
                // it -- this is what makes '.signature' answer something.
                if (lhat_is_object_kind(R(b), LHAT_OBJECT_TYPE)) {
                    if (!lhat_is_object_kind(member_key, LHAT_OBJECT_STRING)) {
                        return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                    }
                    const LhatString *asked =
                        (const LhatString *)lhat_as_object(member_key);
                    if (asked->length != 9 ||
                        memcmp(asked->text, "signature", 9) != 0) {
                        return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                    }
                    const LhatRuntimeType *type =
                        (const LhatRuntimeType *)lhat_as_object(R(b));
                    size_t needed = lhat_runtime_type_write(type, NULL, 0);
                    char *text = (char *)lhat_alloc(needed + 1);
                    if (text == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                      at);
                    }
                    lhat_runtime_type_write(type, text, needed + 1);
                    LhatString *written =
                        lhat_string_new(&m->objects, text, needed);
                    lhat_free(text);
                    if (written == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                      at);
                    }
                    SET_R(a, lhat_object((LhatObject *)written));
                    break;
                }
                // 05 の 8.9: a host value answers a registered field off its
                // own bytes -- no host call -- and a registered member out
                // of the type's table the machine bound at install.
                if (lhat_is_hostvalue(R(b))) {
                    const LhatHostValueTag *hv_tag = lhat_as_hostvalue_tag(R(b));
                    const LhatHostValueField *field =
                        hostvalue_field_named(hv_tag, member_key);
                    if (field != NULL) {
                        SET_R(a, lhat_hostvalue_field_value(
                                     m->slots.values + rbase + b + 1, field));
                        break;
                    }
                    const LhatTable *hv_members =
                        hostvalue_members_of(m, hv_tag);
                    if (hv_members == NULL) {
                        return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                      lhat_nil(), at);
                    }
                    SET_R(a, member_written(m, R(b), member_key, hv_members));
                    // 02 の 14.17: and where the library registered none, the
                    // built-in writes the value down -- a host value has no
                    // spelling of its own, so what it answers is its type's
                    // name (value.c's write_value).
                    LhatNativeKind hv_which;
                    if (lhat_is_nil(R(a)) &&
                        builtin_member(R(b), member_key, &hv_which)) {
                        LhatNative *native =
                            lhat_native_new(&m->objects, hv_which, R(b));
                        if (native == NULL) {
                            return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                          lhat_nil(), at);
                        }
                        SET_R(a, lhat_object((LhatObject *)native));
                    }
                    break;
                }
                // 05 の 8.9: a box answers its two members, bound the way
                // 14.17's tostring is; any other name falls through to the
                // built-ins every value answers. 8.9改: a registered field
                // reads straight off the box's bytes first, as it does off
                // a stack value's.
                if (lhat_is_object_kind(R(b), LHAT_OBJECT_HOSTVALUE_BOX)) {
                    const LhatHostValueBox *box =
                        (const LhatHostValueBox *)lhat_as_object(R(b));
                    const LhatHostValueField *field = hostvalue_field_named(
                        lhat_hostvalue_box_tag(box), member_key);
                    if (field != NULL) {
                        SET_R(a, lhat_hostvalue_field_value(box->run + 1, field));
                        break;
                    }
                    LhatNativeKind which;
                    if (box_member_named(member_key, &which)) {
                        LhatNative *native =
                            lhat_native_new(&m->objects, which, R(b));
                        if (native == NULL) {
                            return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                          lhat_nil(), at);
                        }
                        SET_R(a, lhat_object((LhatObject *)native));
                        break;
                    }
                }
                const LhatTable *table = readable_table(R(b));
                if (table == NULL) {
                    // 02 の 14.17: nil^, bool^, number^ and string^ hold no
                    // members of their own, but every value can be written
                    // down, and 14.17改2 reads a number^ back out of a
                    // string^. Nothing else reaches a value that is not a
                    // table, so these two are the whole of what one answers
                    // -- iterate stays the table path's, which is where a
                    // value carrying fields comes through.
                    LhatNativeKind bare;
                    if (builtin_member(R(b), member_key, &bare) &&
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
                        LhatNative *native =
                            lhat_native_new(&m->objects, bare, R(b));
                        if (native == NULL) {
                            return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                          lhat_nil(), at);
                        }
                        SET_R(a, lhat_object((LhatObject *)native));
                        break;
                    }
                    // 02 の 14.18: and a string^ answers how long it is,
                    // without a call being written. Two readings of the same
                    // bytes: the code points they spell, and how many there
                    // are. 14.18改: the bare word alone, since a string^ has
                    // no names of its own for a hat to be keeping this off.
                    bool counted_hatted = false;
                    CountedKind counted = counted_named(member_key, &counted_hatted);
                    if (counted != COUNTED_NONE && !counted_hatted &&
                        lhat_is_object_kind(R(b), LHAT_OBJECT_STRING)) {
                        const LhatString *text =
                            (const LhatString *)lhat_as_object(R(b));
                        if (counted == COUNTED_COUNT) {
                            // A string is not a collection of elements.
                            return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                          lhat_nil(), at);
                        }
                        size_t held = counted == COUNTED_SIZE
                                          ? text->length
                                          : lhat_string_characters(text);
                        SET_R(a, lhat_integer((int64_t)held));
                        break;
                    }
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                // 05 の 8.9改: a bare host value asks by its bytes of the
                // moment -- the stored keys are sealed boxes, and content
                // is what a box member_key means.
                if (lhat_is_hostvalue(member_key)) {
                    SET_R(a, lhat_table_get_by_value(
                                 table, lhat_as_hostvalue_tag(member_key),
                                 m->slots.values + rbase + cc + 1));
                    break;
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
                    bool plain = plain_table(R(b));
                    if (!lhat_is_nil(through)) {
                        SET_R(b, through);
                    }
                    SET_R(a, got);
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
                SET_R(a, member_written(m, R(b), member_key, table));
            member_answered:;

                // 16.3: a table has an iterate of its own, but only where
                // nothing was written under that name. 14.17 gives tostring
                // the same rule, which this order is already the whole of.
                LhatNativeKind which;
                if (lhat_is_nil(R(a)) &&
                    builtin_member(R(b), member_key, &which)) {
                    LhatNative *native =
                        lhat_native_new(&m->objects, which, R(b));
                    if (native == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                    SET_R(a, lhat_object((LhatObject *)native));
                    break;
                }

                // 02 の 14.18: how long the run is, and how much the table
                // holds altogether. Same order again -- a written one wins,
                // and 14.18 answers only where nothing was. The hat spelling
                // alone: on a table the bare word is the writer's, whatever
                // kind of table it is.
                bool counting_hatted = false;
                CountedKind counting = COUNTED_NONE;
                if (lhat_is_nil(R(a))) {
                    counting = counted_named(member_key, &counting_hatted);
                    if (!counting_hatted) {
                        counting = COUNTED_NONE;
                    }
                }
                if (counting == COUNTED_SIZE) {
                    // Bytes are a reading of a string^, not of a table.
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                if (counting != COUNTED_NONE) {
                    size_t held = counting == COUNTED_COUNT
                                      ? lhat_table_count(table)
                                      : lhat_table_length(table);
                    SET_R(a, lhat_integer((int64_t)held));
                }
                break;
            }

            // 02 の 13.10: the position a destructuring bind just read.
            // 04 の 11.3 spells absence nil^, so a position answering nil^
            // is a position the value does not have, and 13.10 makes a count
            // that does not agree an error rather than Lua's list
            // adjustment. Under strict the checker has already said so and
            // the compiler leaves this out; this is where relaxed and an
            // unchecked compile land.
            // 02 の 13.8改: what stands between an error (or a plain value)
            // arriving where a run was expected and the slots after it being
            // read as positions that were never written.
            case LHAT_BC_CHECKRUN: {
                LhatValue head = R(a);
                // 13.8改: the short side of a widened fold arrives narrow --
                // one value, or a run of fewer positions -- and the missing
                // positions are nil^, which is exactly what the folded type
                // said of them ('(A, B)|C' is '(A|C, B|nil^)'). They are
                // padded here, into the slots the binding reserved. An arm a
                // construct discriminates -- nil^, an error -- never folds,
                // so meeting one here is the mismatch it always was; a run
                // wider than the reservation stays one too.
                if (!lhat_is_run(head)) {
                    if (lhat_is_nil(head) ||
                        lhat_is_object_kind(head, LHAT_OBJECT_ERROR) ||
                        lhat_is_hostvalue(head)) {
                        return finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                      lhat_nil(), at);
                    }
                    if (rbase + a + (size_t)b >= LHAT_STACK_SLOTS) {
                        return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW,
                                      lhat_nil(), at);
                    }
                    SET_R(a + 1, head);
                    for (size_t i = 2; i <= (size_t)b; i++) {
                        SET_R(a + i, lhat_nil());
                    }
                    SET_R(a, lhat_run_head((size_t)b));
                    break;
                }
                size_t width = lhat_run_width(head);
                if (width == (size_t)b) {
                    break;
                }
                if (width > (size_t)b ||
                    rbase + a + (size_t)b >= LHAT_STACK_SLOTS) {
                    return finish(m, chunk, LHAT_RUN_TUPLE_ARITY, lhat_nil(),
                                  at);
                }
                for (size_t i = width + 1; i <= (size_t)b; i++) {
                    SET_R(a + i, lhat_nil());
                }
                SET_R(a, lhat_run_head((size_t)b));
                break;
            }

            // 02 の 13.8改: a tuple written as a value. The positions are in
            // the slots above already -- the head goes down over them, and
            // from here the run is the one every other path produces.
            case LHAT_BC_MAKERUN: {
                if (b < 2 || (size_t)b > LHAT_MAX_TUPLE ||
                    rbase + a + (size_t)b >= LHAT_STACK_SLOTS) {
                    return finish(m, chunk, LHAT_RUN_TUPLE_ARITY, lhat_nil(),
                                  at);
                }
                SET_R(a, lhat_run_head((size_t)b));
                break;
            }

            // 02 の 13.8改: pack^ -- the one bridge from a tuple to a table.
            // 14.10 numbers positions from 1, which is what a destructuring
            // and 't[1]' both read.
            case LHAT_BC_PACK: {
                if (!lhat_is_run(R(a)) || lhat_run_width(R(a)) != (size_t)b) {
                    return finish(m, chunk, LHAT_RUN_TUPLE_ARITY, lhat_nil(),
                                  at);
                }
                LhatTable *packed = lhat_table_new(&m->objects);
                if (packed == NULL) {
                    return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                  at);
                }
                for (size_t i = 0; i < (size_t)b; i++) {
                    // The key is a positive integer every time, so `refused`
                    // (04 の 11.3's nil^, a NaN) cannot come back set.
                    bool refused = false;
                    if (!set_key(m, packed, lhat_integer((int64_t)i + 1),
                                 R(a + 1 + i), &refused)) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                      lhat_nil(), at);
                    }
                }
                SET_R(a, lhat_object((LhatObject *)packed));
                break;
            }

            case LHAT_BC_SETINDEX: {
            set_index:;
                // 05 の 8.9: 'v.x := n' writes the field's bytes in place --
                // the owner register IS the value, so the write lands in the
                // very slots the name holds. Only a registered field takes a
                // write; the members are the host's (8.8's rule holds).
                if (lhat_is_hostvalue(R(a))) {
                    const LhatHostValueField *field = hostvalue_field_named(
                        lhat_as_hostvalue_tag(R(a)), R(b));
                    if (field == NULL ||
                        !hostvalue_field_set(
                            m->slots.values + rbase + a + 1, field, R(cc))) {
                        return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                      lhat_nil(), at);
                    }
                    break;
                }
                LhatTable *table = table_of(R(a));
                if (table == NULL) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                // 05 の 8.6: the machine's own tables are written by
                // the host, not by an instruction. check.c refuses the ones it
                // can name, but a t^{ … } parameter carries no mark of this,
                // so the same question is asked once more where the write
                // actually happens.
                if (table->sealed) {
                    return finish(m, chunk, LHAT_RUN_SEALED, lhat_nil(), at);
                }
                bool refused = false;
                if (!set_key(m, table, R(b), R(cc), &refused)) {
                    return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                // nil^ is how 11.3 spells "not there", so it cannot also be
                // a key. Neither can a NaN, which is not equal to itself.
                if (refused) {
                    return finish(m, chunk, LHAT_RUN_BAD_KEY, lhat_nil(), at);
                }
                break;
            }

            // 14.12: the name keeps what it had and gains another way to be
            // called. What was there may already be a group, or the first of
            // two, or nothing when the base did not define it.
            case LHAT_BC_ADDOVERLOAD: {
                LhatTable *table = table_of(R(a));
                if (table == NULL) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                LhatValue held = lhat_table_get(table, R(b));
                LhatOverload *group = NULL;
                if (lhat_is_object_kind(held, LHAT_OBJECT_OVERLOAD)) {
                    group = (LhatOverload *)lhat_as_object(held);
                } else {
                    group = lhat_overload_new(&m->objects);
                    if (group == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                    if (!lhat_is_nil(held) && !lhat_overload_add(group, held)) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                    bool refused = false;
                    if (!set_key(m, table, R(b),
                                 lhat_object((LhatObject *)group), &refused)) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                }
                if (!lhat_overload_add(group, R(cc))) {
                    return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                // 5.12: `group` may be one already in the table, black since
                // an earlier step. A group gains arms one at a time, so the
                // forward barrier is the cheaper of the two.
                lhat_gc_barrier(m, (LhatObject *)group, R(cc));
                break;
            }

            // 14.12: an override^ replaces the one arm it overlaps, and the
            // arms an overload^ put there are otherwise untouched. A plain
            // write would take the whole group with them.
            case LHAT_BC_OVERRIDEINDEX: {
                LhatTable *table = table_of(R(a));
                if (table == NULL) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                LhatValue held = lhat_table_get(table, R(b));
                if (lhat_is_object_kind(held, LHAT_OBJECT_OVERLOAD)) {
                    const LhatOverload *group =
                        (const LhatOverload *)lhat_as_object(held);
                    LhatOverload *made = lhat_overload_with_first(
                        &m->objects, group, R(cc));
                    if (made == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                      at);
                    }
                    bool refused = false;
                    if (!set_key(m, table, R(b),
                                 lhat_object((LhatObject *)made), &refused)) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                      at);
                    }
                    break;
                }
                goto set_index;
            }

            // 03 の 5.11c: the same write with the arm named. What it replaces
            // is dropped rather than shadowed, so the arms left are the arms
            // the checker's type says the name carries -- which is what makes
            // an arm index mean the same thing on both sides. super^ is
            // unaffected: it was bound from the old group before this ran.
            case LHAT_BC_OVERRIDEARM: {
                LhatTable *table = table_of(R(a));
                if (table == NULL) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                LhatValue held = lhat_table_get(table, R(b));
                if (lhat_is_object_kind(held, LHAT_OBJECT_OVERLOAD)) {
                    const LhatOverload *group =
                        (const LhatOverload *)lhat_as_object(held);
                    LhatOverload *made = lhat_overload_replacing(
                        &m->objects, group, cc, R(b + 1));
                    if (made == NULL) {
                        // No such arm. The group is not the one the checker
                        // read, so the honest answer is 14.12's own: put the
                        // replacement in front and let the search sort it out.
                        made = lhat_overload_with_first(&m->objects, group,
                                                        R(b + 1));
                    }
                    if (made == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                      at);
                    }
                    bool refused = false;
                    if (!set_key(m, table, R(b),
                                 lhat_object((LhatObject *)made), &refused)) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                      at);
                    }
                    break;
                }
                // Nothing overloaded under the name, so this is a plain write.
                // set_index cannot be jumped to for it: that reads the value
                // from C, which here is the arm.
                if (table->sealed) {
                    return finish(m, chunk, LHAT_RUN_SEALED, lhat_nil(), at);
                }
                bool refused = false;
                if (!set_key(m, table, R(b), R(b + 1), &refused)) {
                    return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                if (refused) {
                    return finish(m, chunk, LHAT_RUN_BAD_KEY, lhat_nil(), at);
                }
                break;
            }

            case LHAT_BC_NEWERROR: {
                if (!lhat_is_object_kind(R(b), LHAT_OBJECT_ERROR_KIND)) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                const LhatErrorKind *kind =
                    (const LhatErrorKind *)lhat_as_object(R(b));
                LhatError *error = lhat_error_new(&m->objects, kind);
                if (error == NULL) {
                    return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                }
                SET_R(a, lhat_object((LhatObject *)error));
                break;
            }

            // 04 の 2.6: an error satisfies no type but an error's, so asking
            // whether a value is one is a question about the value alone.
            case LHAT_BC_ISERROR:
                SET_R(a,
                    lhat_bool(lhat_is_object_kind(R(b),
                                                  LHAT_OBJECT_ERROR)));
                break;

            // 02 の 13.11 with 03 の 5.13: R[C] is always a type the
            // compiler lowered -- a written type, never a runtime value --
            // and lhat_value_satisfies answers, the same relation ASCAST and
            // fits_call already trust. (A fallback that read a shape off a
            // definition table at run time: a value that only arrives while
            // the program runs carries no type to ask about.)
            case LHAT_BC_FITS: {
                LhatValue wanted = R(cc);
                if (!lhat_is_object_kind(wanted, LHAT_OBJECT_TYPE)) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                const LhatRuntimeType *type =
                    (const LhatRuntimeType *)lhat_as_object(wanted);
                SET_R(a,
                    lhat_bool(lhat_value_satisfies(R(b), type)));
                break;
            }

            case LHAT_BC_ISNIL:
                SET_R(a, lhat_bool(lhat_is_nil(R(b))));
                break;

            // 14.11: construction is the machine's -- a copy of the
            // prototype the definition's self^ holds, which is where every
            // field starts as its default. A table held in a field is
            // copied as its own tree; a definition among the values is
            // shared. 14.3 and 14.7: the copy holds its own fields and
            // reads the shared members through the link; 14.2 fixes it
            // here and gives no way to change it afterwards.
            case LHAT_BC_NEWINSTANCE: {
                if (!lhat_is_object_kind(R(b), LHAT_OBJECT_TABLE)) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                const LhatTable *definition =
                    (const LhatTable *)lhat_as_object(R(b));
                LhatValue held = lhat_table_get(
                    definition, lhat_object((LhatObject *)m->self_key));
                LhatTable *instance;
                bool too_deep = false;
                if (lhat_is_object_kind(held, LHAT_OBJECT_TABLE)) {
                    instance = clone_table(
                        m, (const LhatTable *)lhat_as_object(held), 0,
                        &too_deep);
                } else {
                    // A table wearing the definition mark without a
                    // prototype -- a host built it. Empty, with the link.
                    instance = lhat_table_new(&m->objects);
                }
                if (instance == NULL) {
                    return finish(m, chunk,
                                  too_deep ? LHAT_RUN_MUTABLE_DEFAULT
                                           : LHAT_RUN_OUT_OF_MEMORY,
                                  lhat_nil(), at);
                }
                instance->definition = definition;
                SET_R(a, lhat_object((LhatObject *)instance));
                break;
            }

            // 14.11: the prototype goes under the definition's self^, sealed
            // -- what it holds is every instance's starting point, and 8.8
            // closes a definition to writes -- and linked, so its own reads
            // reach the members the way any instance's do. The values are
            // baked here because this is where they actually are: a table
            // becomes the prototype's own sealed tree, whatever expression
            // produced it, and what nothing may share is refused -- the same
            // answer the checker gives where it ran.
            // 14.7改2: what the definition delegates to. The key is a chunk
            // constant, so nothing is written into the machine's heap here
            // and no barrier is owed.
            case LHAT_BC_SETDELEGATE: {
                LhatTable *table = table_of(R(a));
                if (table == NULL ||
                    !lhat_is_object_kind(R(b), LHAT_OBJECT_STRING)) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                table->delegate_key = R(b);
                table->delegate_from_self = cc != 0;
                break;
            }

            case LHAT_BC_SETPROTO: {
                LhatTable *table = table_of(R(a));
                LhatTable *proto = table_of(R(b));
                if (table == NULL || proto == NULL) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                for (size_t i = 0; i < proto->array_count; i++) {
                    LhatValue baked = lhat_nil();
                    bool bad = false;
                    if (!bake_default(m, lhat_slots_get(proto->array, i),
                                      &baked, 0, &bad)) {
                        return finish(m, chunk,
                                      bad ? LHAT_RUN_MUTABLE_DEFAULT
                                          : LHAT_RUN_OUT_OF_MEMORY,
                                      lhat_nil(), at);
                    }
                    lhat_slots_set(proto->array, i, baked);
                    lhat_gc_barrier_back(m, (LhatObject *)proto, baked);
                }
                for (size_t i = 0; i < proto->entry_capacity; i++) {
                    LhatTableEntry *entry = &proto->entries[i];
                    if (lhat_is_nil(entry->key)) {
                        continue;
                    }
                    LhatValue baked = lhat_nil();
                    bool bad = false;
                    if (!bake_default(m, entry->value, &baked, 0, &bad)) {
                        return finish(m, chunk,
                                      bad ? LHAT_RUN_MUTABLE_DEFAULT
                                          : LHAT_RUN_OUT_OF_MEMORY,
                                      lhat_nil(), at);
                    }
                    entry->value = baked;
                    lhat_gc_barrier_back(m, (LhatObject *)proto, baked);
                }
                proto->definition = table;
                proto->sealed = true;
                bool refused = false;
                if (!set_key(m, table,
                             lhat_object((LhatObject *)m->self_key),
                             lhat_object((LhatObject *)proto), &refused)) {
                    return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                  at);
                }
                break;
            }

            // 05 の 8.9: the host value at R[B..], boxed. The head slot
            // carries the tag, and the tag the width, so the copy is the
            // whole run.
            case LHAT_BC_BOX: {
                // 8.9改: C bit 0 seals the box (constbox^); bit 1 reads
                // R[B] as a box to copy rather than a value laid out.
                const LhatValueUnion *from;
                const LhatHostValueTag *tag;
                if ((cc & 2) != 0) {
                    if (!lhat_is_object_kind(R(b), LHAT_OBJECT_HOSTVALUE_BOX)) {
                        return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                      lhat_nil(), at);
                    }
                    const LhatHostValueBox *source =
                        (const LhatHostValueBox *)lhat_as_object(R(b));
                    tag = lhat_hostvalue_box_tag(source);
                    from = source->run;
                } else {
                    if (!lhat_is_hostvalue(R(b))) {
                        return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                      lhat_nil(), at);
                    }
                    tag = lhat_as_hostvalue_tag(R(b));
                    from = m->slots.values + rbase + b;
                }
                LhatHostValueBox *box =
                    lhat_hostvalue_box_new(&m->objects, tag);
                if (box == NULL) {
                    return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                  at);
                }
                for (size_t i = 0; i < tag->width; i++) {
                    box->run[i] = from[i];
                }
                box->sealed = (cc & 1) != 0;
                SET_R(a, lhat_object((LhatObject *)box));
                break;
            }

            case LHAT_BC_CALL:
            case LHAT_BC_CALLMETHOD:
            case LHAT_BC_TAILCALL:
            case LHAT_BC_TAILCALLMETHOD: {
                // 14.4: whether the receiver was laid out below the arguments.
                // 5.3: and whether the call may take this frame over rather
                // than push one -- which only the closure path below can do,
                // so everything up to it reads the same either way.
                bool as_method = op == LHAT_BC_CALLMETHOD ||
                                 op == LHAT_BC_TAILCALLMETHOD;
                bool tail = op == LHAT_BC_TAILCALL ||
                            op == LHAT_BC_TAILCALLMETHOD;

                // 14.12: at most one candidate fits, so this is a search and
                // not a choice -- no ranking, no ambiguity to report. It ends
                // at the first that takes what it was given.
                //
                // Settled before anything else looks at the callee: 05 の 8.7
                // lets a registration be one of the arms, so what is chosen
                // may be a host function, and every path below has to see
                // what was chosen rather than the group it came out of.
                if (lhat_is_object_kind(R(a), LHAT_OBJECT_OVERLOAD)) {
                    const LhatOverload *group =
                        (const LhatOverload *)lhat_as_object(R(a));
                    size_t picked_skip = 1;
                    // 2.2: fits_call reads the callee and arguments as one
                    // LhatValue run, which the stack is not -- so the
                    // slots are gathered once and every candidate reads the
                    // same copy.
                    LhatValue lineup[LHAT_MAX_REGISTERS + 2];
                    for (size_t i = 0; i <= (size_t)b + 1; i++) {
                        lineup[i] = R(a + i);
                    }
                    LhatValue chosen = lhat_nil();
                    for (size_t i = 0; i < group->count; i++) {
                        if (fits_call(group->candidates[i], lineup, b,
                                      as_method, &picked_skip)) {
                            chosen = group->candidates[i];
                            break;
                        }
                    }
                    if (lhat_is_nil(chosen)) {
                        return finish(m, chunk, LHAT_RUN_NO_CANDIDATE,
                                      lhat_nil(), at);
                    }
                    SET_R(a, chosen);
                }

                // 05 の 8.7: the host wrote this one in C. 13.1 settled how
                // many arguments there are before anything ran, so they are
                // handed over as they lie rather than pushed one by one.
                // 04 の 12.8 makes an error a value, so what comes back is
                // one -- there is no unwinding to arrange.
                if (lhat_is_object_kind(R(a), LHAT_OBJECT_HOST)) {
                    LhatHost *host = (LhatHost *)lhat_as_object(R(a));
                    size_t skip = as_method ? 2 : 1;

                    // 13.7: 'expr...' wrote a table in the last slot instead
                    // of an ordinary argument. The closure path below unpacks
                    // one into the frame it is about to push; there is no
                    // frame to push here, so it is unpacked into an array of
                    // its own further down.
                    const LhatTable *spread_table = NULL;
                    size_t spread_run = SIZE_MAX;
                    size_t written = b;
                    // 13.8改: C carries the reserved-slot count too, so the
                    // spread is the low bit rather than the whole byte.
                    if ((cc & LHAT_CALL_SPREAD) != 0) {
                        if (b == 0) {
                            return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                          lhat_nil(), at);
                        }
                        LhatValue spread_from = R(a + skip + b - 1);
                        // 13.8改: a tuple spreads as the run it already is.
                        // Its positions are the slots after the head, so
                        // nothing is unpacked -- only counted.
                        if (lhat_is_run(spread_from)) {
                            spread_run = rbase + a + skip + (size_t)b - 1;
                            written =
                                (size_t)b - 1 + lhat_run_width(spread_from);
                        } else {
                            spread_table = table_of(spread_from);
                            if (spread_table == NULL) {
                                return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                              lhat_nil(), at);
                            }
                            written =
                                (size_t)b - 1 + spread_table->array_count;
                        }
                    }

                    // 13.4 keeps self^ out of the parameter list, so what the
                    // call wrote is compared against the list and the
                    // receiver is handed over besides it. 13.7 makes that
                    // comparison a floor once the signature ended in '...'.
                    if (host->has_variadic ? written < host->parameters
                                           : written != host->parameters) {
                        return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(), at);
                    }

                    // 14.4: the receiver comes first, and sits just below the
                    // arguments the call wrote.
                    bool receiver_first =
                        host->takes_self && as_method;
                    size_t given = written + (receiver_first ? 1 : 0);
                    // 05 の 8.9: a host value receiver holds its width of
                    // slots, so a skipped receiver is skipped whole.
                    size_t receiver_width =
                        as_method && lhat_is_hostvalue(R(a + 1))
                            ? lhat_as_hostvalue_tag(R(a + 1))->width
                            : 1;
                    // 2.2: the stack holds no LhatValue run any more, so
                    // the arguments a host receives are gathered into one.
                    // 05 の 8.9: walked by slot rather than by value -- a
                    // host value argument is one argument, its head handed
                    // over aiming into the stack, and its width of slots
                    // stepped past.
                    LhatValue gathered[LHAT_MAX_REGISTERS + 2];
                    size_t slot = receiver_first
                                      ? (size_t)a + 1
                                      : (size_t)a +
                                            (as_method
                                                 ? 1 + receiver_width
                                                 : 1);
                    for (size_t i = 0; i < given; i++) {
                        LhatValue held = R(slot);
                        if (lhat_is_hostvalue(held)) {
                            gathered[i] =
                                hostvalue_argument(m->slots, rbase + slot);
                            slot += lhat_as_hostvalue_tag(held)->width;
                        } else {
                            gathered[i] = held;
                            slot += 1;
                        }
                    }
                    const LhatValue *arguments = gathered;

                    // Packed only where a spread broke the contiguity the
                    // registers otherwise have. An allocation rather than the
                    // stack above this frame, because a host function may call
                    // back in and lhat_machine_call starts its frame exactly
                    // there. Nothing in it needs rooting: every value is also
                    // a position of `spread_table` or a register of this
                    // frame, and both stay reachable for as long as the call
                    // does.
                    LhatValue *packed = NULL;
                    if ((spread_table != NULL || spread_run != SIZE_MAX) &&
                        given > 0) {
                        packed = (LhatValue *)lhat_alloc(given * sizeof *packed);
                        if (packed == NULL) {
                            return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                          lhat_nil(), at);
                        }
                        size_t into = 0;
                        if (receiver_first) {
                            packed[into++] = R(a + 1);
                        }
                        for (size_t i = 0; i < written; i++) {
                            packed[into++] = call_arg(m->slots, rbase, a, skip,
                                                      spread_table, spread_run,
                                                      (size_t)b - 1, i);
                        }
                        arguments = packed;
                    }

                    // 05 の 8.8: a dispose^ written by hand is the same
                    // giving-back the collection would do, so it is marked
                    // here and 10.7 keeps the sweep from doing it again.
                    if (host->takes_self && given > 0 &&
                        lhat_is_object_kind(arguments[0],
                                            LHAT_OBJECT_HOSTDATA)) {
                        LhatHostData *data =
                            (LhatHostData *)lhat_as_object(arguments[0]);
                        // 8.8改: the nearest release on the chain, since an
                        // inherited dispose^ is the type's own way back.
                        const LhatHostDataTag *by =
                            lhat_hostdata_releaser(data->tag);
                        if (by != NULL && by->release == host->call) {
                            data->released = true;
                        }
                    }
                    // 04 の 11.6改: the host may read the frames while it
                    // runs, and this frame's line is read off its saved pc.
                    frame->pc = pc;
                    size_t frames_before = m->frame_count;
                    LhatValue answered = lhat_nil();
                    bool said = call_host_fn(m, host->call, host->context,
                                             arguments, given, &answered);
                    lhat_free(packed);
                    if (!said) {
                        return finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                      lhat_nil(), at);
                    }
                    // 05 の 8.7改 and 8.7改2: a nested run that faulted, or
                    // a panic the host asked for, ends this run here --
                    // running on would stack this frame's own calls over
                    // the standing ones, and the traceback keeps the chain.
                    LhatRunStatus left = LHAT_RUN_OK;
                    LhatValue left_with = lhat_nil();
                    if (host_faulted(m, frames_before, &left, &left_with)) {
                        return finish(m, chunk, left, left_with, at);
                    }
                    // 05 の 8.9: a host value answer arrives as a
                    // head-shaped run and is written out whole on the spot,
                    // before anything else can touch the scratch it may live
                    // in.
                    if (lhat_is_hostvalue(answered)) {
                        // 05 の 8.9: the call site said the width it made
                        // room for (compile's prepared); a site that could
                        // not know the type reserved one slot, and writing
                        // past it would eat the neighbours -- refused, as
                        // every width the types never settled is.
                        const LhatValueUnion *arun = answered.as.hostvalue_run;
                        if (arun == NULL || arun[0].hostvalue == NULL ||
                            lhat_call_prepared(cc) <
                                arun[0].hostvalue->width ||
                            !place_hostvalue_answer(m, rbase + a, answered)) {
                            return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                          lhat_nil(), at);
                        }
                        break;
                    }
                    // 02 の 13.8改: and the several values a host answers
                    // with go down the same way. Until this, the host path
                    // was the one caller that never read C's reserved count
                    // -- a registration saying '-> (A, B)' compiled to a
                    // reservation nothing filled, and the slots behind the
                    // head kept whatever the frame last had there.
                    size_t room = lhat_call_prepared(cc);
                    if (lhat_is_run(answered)) {
                        if (!place_run_answer(m, rbase + a, room, answered)) {
                            return finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                          lhat_nil(), at);
                        }
                        break;
                    }
                    // Whatever the host may have staged, it did not answer
                    // with it, so the room is free again.
                    m->tuple_scratch_count = 0;
                    // 13.8改 with 04 の 3.1, and 05 の 8.9改: room past one
                    // slot says the site reserved for something wide -- a
                    // run ('-> (A, B)'), or a host value's own width
                    // ('-> Vector3|nil^'). One value coming back fills it
                    // only when that value is the arm the head slot tells
                    // apart: an error for a try^ to find, or a nil^ for a
                    // '??'. Anything else is a registration promising a
                    // shape its C never answers with, and the promise is
                    // the host's own -- so it faults here rather than
                    // leaving the slots behind the head as they were.
                    if (room > 1 && !lhat_is_nil(answered) &&
                        !lhat_is_object_kind(answered, LHAT_OBJECT_ERROR)) {
                        return finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                      lhat_nil(), at);
                    }
                    SET_R(a, answered);
                    break;
                }

                // 02 の 12.6 and 15.6: resume and dispose are the runtime's,
                // not the program's, so they are performed rather than called.
                if (lhat_is_object_kind(R(a), LHAT_OBJECT_NATIVE)) {
                    const LhatNative *native =
                        (const LhatNative *)lhat_as_object(R(a));
                    size_t first = a + (as_method ? 2 : 1);
                    LhatValue sent = b > 0 ? R(first) : lhat_nil();

                    // 05 の 8.6: the one thing a program cannot arrange for
                    // itself. It takes nothing and answers nothing.
                    if (native->kind == LHAT_NATIVE_COLLECTGARBAGE) {
                        if (b != 0) {
                            return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(), at);
                        }
                        lhat_gc_collect(m);
                        SET_R(a, lhat_nil());
                        break;
                    }

                    // 05 の 8.9: the box's two members. get answers the value
                    // whole -- the box's run is head-shaped, so the placement
                    // a host's answer takes lays it down unchanged. set
                    // writes a value of the same tag over the bytes.
                    // 02 の 14.22: the table operations, through one door.
                    // The arguments are copied out of the window first -- a
                    // sort's comparator runs nested L^ code over this frame
                    // -- and the answer lands over R(a) afterwards.
                    if (native->kind >= LHAT_NATIVE_JOIN &&
                        native->kind <= LHAT_NATIVE_CLEAR) {
                        LhatValue held[4];
                        if (b > 4) {
                            return finish(m, chunk, LHAT_RUN_ARITY,
                                          lhat_nil(), at);
                        }
                        for (size_t i = 0; i < (size_t)b; i++) {
                            held[i] = R(first + i);
                        }
                        LhatValue answered = lhat_nil();
                        LhatRunStatus asked = table_native(m, native, held,
                                                           (size_t)b,
                                                           &answered);
                        if (asked != LHAT_RUN_OK) {
                            return finish(m, chunk, asked, lhat_nil(), at);
                        }
                        SET_R(a, answered);
                        break;
                    }

                    if (native->kind == LHAT_NATIVE_BOX_GET ||
                        native->kind == LHAT_NATIVE_BOX_SET) {
                        LhatHostValueBox *box =
                            (LhatHostValueBox *)lhat_as_object(native->bound);
                        const LhatHostValueTag *box_tag =
                            lhat_hostvalue_box_tag(box);
                        if (native->kind == LHAT_NATIVE_BOX_GET) {
                            if (b != 0) {
                                return finish(m, chunk, LHAT_RUN_ARITY,
                                              lhat_nil(), at);
                            }
                            LhatValue answered;
                            answered.tag = LHAT_VALUE_HOSTVALUE;
                            answered.as.hostvalue_run = box->run;
                            // As at a host call's answer: the site has to
                            // have reserved the width.
                            if (lhat_call_prepared(cc) < box_tag->width ||
                                !place_hostvalue_answer(m, rbase + a,
                                                        answered)) {
                                return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                              lhat_nil(), at);
                            }
                            break;
                        }
                        if (b != 1) {
                            return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(),
                                          at);
                        }
                        if (!lhat_is_hostvalue(sent) ||
                            lhat_as_hostvalue_tag(sent) != box_tag) {
                            return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                          lhat_nil(), at);
                        }
                        // 14.11: a prototype's box takes no writes.
                        if (box->sealed) {
                            return finish(m, chunk, LHAT_RUN_SEALED,
                                          lhat_nil(), at);
                        }
                        for (size_t i = 1; i < box_tag->width; i++) {
                            box->run[i] = m->slots.values[rbase + first + i];
                        }
                        SET_R(a, lhat_nil());
                        break;
                    }

                    // 02 の 14.17: the value written down. Takes nothing, or
                    // a format when what it is bound to is a number^ -- the
                    // two signatures 14.12 makes an intersection of, told
                    // apart here by how many arguments arrived.
                    if (native->kind == LHAT_NATIVE_TOSTRING) {
                        if (b > 1) {
                            return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(),
                                          at);
                        }
                        size_t needed;
                        char *text;
                        if (b == 1) {
                            if (!lhat_is_number(native->bound)) {
                                // Only a number^ carries the second
                                // signature, so this call was never one of
                                // the two ways of writing this value.
                                return finish(m, chunk, LHAT_RUN_ARITY,
                                              lhat_nil(), at);
                            }
                            LhatValue fmt = sent;
                            if (!lhat_is_object_kind(fmt, LHAT_OBJECT_STRING)) {
                                return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                              lhat_nil(), at);
                            }
                            const LhatString *spelt =
                                (const LhatString *)lhat_as_object(fmt);
                            if (!lhat_number_format(native->bound, spelt->text,
                                                    spelt->length, NULL, 0,
                                                    &needed)) {
                                return finish(m, chunk, LHAT_RUN_BAD_FORMAT,
                                              lhat_nil(), at);
                            }
                            text = (char *)lhat_alloc(needed + 1);
                            if (text == NULL) {
                                return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                              lhat_nil(), at);
                            }
                            lhat_number_format(native->bound, spelt->text,
                                               spelt->length, text, needed + 1,
                                               &needed);
                        } else {
                            // 05 の 8.9: a bound host value is the head
                            // alone -- the tag, no bytes -- so the name is
                            // all there is to write. lhat_value_text reads
                            // the pointer form (a host's argument) and must
                            // not see this one.
                            if (lhat_is_hostvalue(native->bound)) {
                                const LhatHostValueTag *bound_tag =
                                    lhat_as_hostvalue_tag(native->bound);
                                size_t module_len = strlen(bound_tag->module);
                                size_t name_len = strlen(bound_tag->name);
                                needed = module_len + name_len + 3;
                                text = (char *)lhat_alloc(needed + 1);
                                if (text == NULL) {
                                    return finish(m, chunk,
                                                  LHAT_RUN_OUT_OF_MEMORY,
                                                  lhat_nil(), at);
                                }
                                snprintf(text, needed + 1, "<%s.%s>",
                                         bound_tag->module, bound_tag->name);
                                LhatString *spelt_name = lhat_string_new(
                                    &m->objects, text, needed);
                                lhat_free(text);
                                if (spelt_name == NULL) {
                                    return finish(m, chunk,
                                                  LHAT_RUN_OUT_OF_MEMORY,
                                                  lhat_nil(), at);
                                }
                                SET_R(a, lhat_object((LhatObject *)spelt_name));
                                break;
                            }
                            needed = lhat_value_text(native->bound, NULL, 0);
                            text = (char *)lhat_alloc(needed + 1);
                            if (text == NULL) {
                                return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                              lhat_nil(), at);
                            }
                            lhat_value_text(native->bound, text, needed + 1);
                        }
                        LhatString *written =
                            lhat_string_new(&m->objects, text, needed);
                        lhat_free(text);
                        if (written == NULL) {
                            return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                          lhat_nil(), at);
                        }
                        SET_R(a, lhat_object((LhatObject *)written));
                        break;
                    }

                    // 02 の 14.20: the comparison '=' makes, with the error
                    // term written down instead of taken from 14.8. The same
                    // predicate either way, so what a writer chooses is the
                    // width of the band and never a different question.
                    if (native->kind == LHAT_NATIVE_EQ) {
                        if (b != 2) {
                            return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(),
                                          at);
                        }
                        LhatValue against = sent;
                        LhatValue width = R(first + 1);
                        if (!lhat_is_number(against) ||
                            !lhat_is_number(width)) {
                            return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                          lhat_nil(), at);
                        }
                        // Written as a distance rather than as 14.8's factor:
                        // a writer asking for one says how far apart two may
                        // be, which is the number they have in hand. Scaled
                        // the same way all the same, so the answer does not
                        // change with where on the line the two sit.
                        double allowed = lhat_number_as_real(width);
                        SET_R(a, lhat_bool(lhat_value_close(
                                     native->bound, against, allowed)));
                        break;
                    }

                    // 02 の 14.21: the whole number below, above or nearest.
                    // Nothing to take: which of the three it is was settled
                    // by the name the member was reached through.
                    if (native->kind == LHAT_NATIVE_FLOOR ||
                        native->kind == LHAT_NATIVE_CEIL ||
                        native->kind == LHAT_NATIVE_ROUND) {
                        if (b != 0) {
                            return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(),
                                          at);
                        }
                        SET_R(a, whole_of(native->bound,
                                          native->kind == LHAT_NATIVE_FLOOR
                                              ? floor
                                          : native->kind == LHAT_NATIVE_CEIL
                                              ? ceil
                                              : nearbyint));
                        break;
                    }

                    // 02 の 14.21改: abs and sign take nothing; clamp takes
                    // its two bounds. An integer stays an integer (14.8改)
                    // -- only the one integer with no negative widens.
                    if (native->kind == LHAT_NATIVE_ABS ||
                        native->kind == LHAT_NATIVE_SIGN) {
                        if (b != 0) {
                            return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(),
                                          at);
                        }
                        LhatValue self = native->bound;
                        if (native->kind == LHAT_NATIVE_SIGN) {
                            double d = lhat_number_as_real(self);
                            SET_R(a, lhat_integer(d > 0 ? 1 : d < 0 ? -1 : 0));
                        } else if (lhat_is_integer(self)) {
                            int64_t i = lhat_as_integer(self);
                            SET_R(a, i == INT64_MIN
                                         ? lhat_real(-(double)i)
                                         : lhat_integer(i < 0 ? -i : i));
                        } else {
                            SET_R(a, lhat_real(fabs(lhat_as_real(self))));
                        }
                        break;
                    }
                    if (native->kind == LHAT_NATIVE_CLAMP) {
                        if (b != 2) {
                            return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(),
                                          at);
                        }
                        LhatValue low = sent;
                        LhatValue high = R(first + 1);
                        if (!lhat_is_number(low) || !lhat_is_number(high)) {
                            return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                          lhat_nil(), at);
                        }
                        // The answer is one of the three as handed over, so
                        // an integer bound keeps its representation.
                        LhatValue self = native->bound;
                        double d = lhat_number_as_real(self);
                        SET_R(a, d < lhat_number_as_real(low)    ? low
                                 : d > lhat_number_as_real(high) ? high
                                                                 : self);
                        break;
                    }

                    // 02 の 14.17改2: the number^ a string^ names, or nil^
                    // where it names none. Takes nothing, or a format -- the
                    // same two signatures 14.17's takes, told apart the same
                    // way, by how many arguments arrived.
                    if (native->kind == LHAT_NATIVE_TONUMBER) {
                        if (b > 1) {
                            return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(),
                                          at);
                        }
                        // builtin_member only ever binds this to a string^.
                        const LhatString *subject =
                            (const LhatString *)lhat_as_object(native->bound);
                        if (b == 0) {
                            // 01 の 10 章's own grammar, so what tonumber
                            // reads and what L^ reads cannot drift apart.
                            bool is_real = false;
                            int64_t whole = 0;
                            double real = 0.0;
                            if (!lhat_number_read(subject->text,
                                                  subject->length, &is_real,
                                                  &whole, &real)) {
                                SET_R(a, lhat_nil());
                                break;
                            }
                            SET_R(a, is_real ? lhat_real(real)
                                             : lhat_integer(whole));
                            break;
                        }
                        if (!lhat_is_object_kind(sent, LHAT_OBJECT_STRING)) {
                            return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                          lhat_nil(), at);
                        }
                        const LhatString *spelt =
                            (const LhatString *)lhat_as_object(sent);
                        LhatValue number = lhat_nil();
                        bool got = false;
                        if (!lhat_number_scan(subject->text, subject->length,
                                              spelt->text, spelt->length,
                                              &number, &got)) {
                            // 14.17 draws the line in the same place: the
                            // format is the writer's and a bad one is an
                            // error, where the text is data and a text that
                            // does not match is simply not a number^.
                            return finish(m, chunk, LHAT_RUN_BAD_FORMAT,
                                          lhat_nil(), at);
                        }
                        SET_R(a, got ? number : lhat_nil());
                        break;
                    }

                    // 02 の 14.19: a run of the subject's characters, named
                    // by ordinals that start at 1 and count from the end
                    // when negative. A range that does not stand answers the
                    // empty string -- what is not there is not an error, the
                    // way 04 の 11.3 has a missing key answer nil^.
                    //
                    // 14.19改: at(i) is that run with both ends at i, so it
                    // comes through here -- the ordinal is resolved, rounded
                    // and refused in exactly the same places.
                    if (native->kind == LHAT_NATIVE_SUBSTRING ||
                        native->kind == LHAT_NATIVE_AT) {
                        bool single = native->kind == LHAT_NATIVE_AT;
                        if (b < 1 || b > (single ? 1 : 2)) {
                            return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(),
                                          at);
                        }
                        const LhatString *subject =
                            (const LhatString *)lhat_as_object(native->bound);
                        int64_t from = 0;
                        int64_t to = 0;
                        if (!ordinal_of(sent, &from) ||
                            (b == 2 && !ordinal_of(R(first + 1), &to))) {
                            // Handing over something that is not a number is
                            // the writer's mistake, not a range that came out
                            // empty -- 14.17改2 draws the same line.
                            return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                          lhat_nil(), at);
                        }
                        size_t count = subject->characters;
                        // The one ordinal ends where it starts for at, and at
                        // the end of the string for substring.
                        int64_t last = single  ? from
                                       : b == 2 ? to
                                                : (int64_t)count;
                        int64_t start = resolve_ordinal(from, count);
                        int64_t end = resolve_ordinal(last, count);
                        if (start < 1 || end < start || end > (int64_t)count) {
                            LhatString *empty =
                                lhat_string_new(&m->objects, "", 0);
                            if (empty == NULL) {
                                return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                              lhat_nil(), at);
                            }
                            SET_R(a, lhat_object((LhatObject *)empty));
                            break;
                        }
                        // The whole of it is the string itself: nothing about
                        // a string changes, so a copy would be a second name
                        // for the same bytes and nothing more.
                        if (start == 1 && end == (int64_t)count) {
                            SET_R(a, native->bound);
                            break;
                        }
                        size_t at_byte =
                            lhat_string_byte_at(subject, (size_t)start - 1);
                        size_t end_byte =
                            lhat_string_byte_at(subject, (size_t)end);
                        LhatString *cut = lhat_string_new(
                            &m->objects, subject->text + at_byte,
                            end_byte - at_byte);
                        if (cut == NULL) {
                            return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                          lhat_nil(), at);
                        }
                        SET_R(a, lhat_object((LhatObject *)cut));
                        break;
                    }

                    // 02 の 14.19改3: the ASCII case swaps. The rest of the
                    // bytes pass through as they are -- no Unicode case
                    // tables -- and a string nothing changed in answers
                    // itself, substring's whole-of-it economy.
                    if (native->kind == LHAT_NATIVE_TOUPPER ||
                        native->kind == LHAT_NATIVE_TOLOWER) {
                        if (b != 0) {
                            return finish(m, chunk, LHAT_RUN_ARITY,
                                          lhat_nil(), at);
                        }
                        const LhatString *subject =
                            (const LhatString *)lhat_as_object(native->bound);
                        bool up = native->kind == LHAT_NATIVE_TOUPPER;
                        size_t changed = 0;
                        for (size_t i = 0; i < subject->length; i++) {
                            char head = subject->text[i];
                            if (up ? (head >= 'a' && head <= 'z')
                                   : (head >= 'A' && head <= 'Z')) {
                                changed++;
                            }
                        }
                        if (changed == 0) {
                            SET_R(a, native->bound);
                            break;
                        }
                        char *text = (char *)lhat_alloc(subject->length + 1);
                        if (text == NULL) {
                            return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                          lhat_nil(), at);
                        }
                        for (size_t i = 0; i < subject->length; i++) {
                            char head = subject->text[i];
                            if (up && head >= 'a' && head <= 'z') {
                                head = (char)(head - 'a' + 'A');
                            } else if (!up && head >= 'A' && head <= 'Z') {
                                head = (char)(head - 'A' + 'a');
                            }
                            text[i] = head;
                        }
                        LhatString *swapped = lhat_string_new(
                            &m->objects, text, subject->length);
                        lhat_free(text);
                        if (swapped == NULL) {
                            return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                          lhat_nil(), at);
                        }
                        SET_R(a, lhat_object((LhatObject *)swapped));
                        break;
                    }

                    // 02 の 14.19改3: join^'s inverse, so the law decides
                    // the details -- s.split(sep).join^(sep) = s, which is
                    // what keeps every empty piece. The 0-argument form is
                    // the other reading, "the words": runs of whitespace
                    // split it and nothing empty is kept.
                    if (native->kind == LHAT_NATIVE_SPLIT) {
                        if (b > 1 ||
                            (b == 1 &&
                             !lhat_is_object_kind(sent, LHAT_OBJECT_STRING))) {
                            return finish(m, chunk,
                                          b > 1 ? LHAT_RUN_ARITY
                                                : LHAT_RUN_TYPE_ERROR,
                                          lhat_nil(), at);
                        }
                        const LhatString *subject =
                            (const LhatString *)lhat_as_object(native->bound);
                        LhatTable *pieces = lhat_table_new(&m->objects);
                        if (pieces == NULL) {
                            return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                          lhat_nil(), at);
                        }
                        // The table under construction is a root while the
                        // strings are made (14.22's chain).
                        LhatNativeHold hold;
                        hold.held = lhat_object((LhatObject *)pieces);
                        hold.outer = m->native_hold;
                        m->native_hold = &hold;
                        bool refused = false;
                        bool ok = true;
                        int64_t position = 0;
                        if (b == 0) {
                            // The words: whitespace runs split, empties drop.
                            size_t i = 0;
                            while (ok && i < subject->length) {
                                while (i < subject->length &&
                                       ((unsigned char)subject->text[i] <=
                                        ' ')) {
                                    i++;
                                }
                                size_t begin = i;
                                while (i < subject->length &&
                                       ((unsigned char)subject->text[i] >
                                        ' ')) {
                                    i++;
                                }
                                if (i == begin) {
                                    break;
                                }
                                LhatString *piece = lhat_string_new(
                                    &m->objects, subject->text + begin,
                                    i - begin);
                                ok = piece != NULL &&
                                     set_key(m, pieces,
                                             lhat_integer(++position),
                                             lhat_object((LhatObject *)piece),
                                             &refused);
                            }
                        } else {
                            const LhatString *sep =
                                (const LhatString *)lhat_as_object(sent);
                            size_t from = 0;
                            while (ok) {
                                size_t found = 0;
                                bool hit = false;
                                size_t end;
                                if (sep->length == 0) {
                                    // One character per piece -- and the
                                    // round trip with join^("") holds.
                                    if (from >= subject->length) {
                                        break;
                                    }
                                    end = from + 1;
                                    while (end < subject->length &&
                                           ((unsigned char)subject
                                                    ->text[end] &
                                            0xC0) == 0x80) {
                                        end++;
                                    }
                                } else {
                                    hit = find_bytes(subject->text,
                                                     subject->length, from,
                                                     sep->text, sep->length,
                                                     &found);
                                    end = hit ? found : subject->length;
                                }
                                LhatString *piece = lhat_string_new(
                                    &m->objects, subject->text + from,
                                    end - from);
                                ok = piece != NULL &&
                                     set_key(m, pieces,
                                             lhat_integer(++position),
                                             lhat_object((LhatObject *)piece),
                                             &refused);
                                if (!ok) {
                                    break;
                                }
                                if (sep->length == 0) {
                                    from = end;
                                } else if (!hit) {
                                    break;  // the tail piece went in
                                } else {
                                    from = found + sep->length;
                                }
                            }
                        }
                        m->native_hold = hold.outer;
                        if (!ok) {
                            return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                          lhat_nil(), at);
                        }
                        SET_R(a, lhat_object((LhatObject *)pieces));
                        break;
                    }

                    // 02 の 14.19改3: the plain searches. The needle is a
                    // literal string -- what a pattern would ask for lives
                    // in std.regex -- and what is not there answers nil^
                    // (04 の 11.3), never a sentinel.
                    if (native->kind == LHAT_NATIVE_FIND ||
                        native->kind == LHAT_NATIVE_FINDALL ||
                        native->kind == LHAT_NATIVE_REPLACE) {
                        const LhatString *subject =
                            (const LhatString *)lhat_as_object(native->bound);
                        if (b < 1 ||
                            !lhat_is_object_kind(sent, LHAT_OBJECT_STRING)) {
                            return finish(m, chunk,
                                          b < 1 ? LHAT_RUN_ARITY
                                                : LHAT_RUN_TYPE_ERROR,
                                          lhat_nil(), at);
                        }
                        const LhatString *needle =
                            (const LhatString *)lhat_as_object(sent);

                        if (native->kind == LHAT_NATIVE_FIND) {
                            if (b > 2) {
                                return finish(m, chunk, LHAT_RUN_ARITY,
                                              lhat_nil(), at);
                            }
                            // The optional second ordinal reads as
                            // substring's does -- 1-based, negative from
                            // the end.
                            int64_t from = 1;
                            if (b == 2 && !ordinal_of(R(first + 1), &from)) {
                                return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                              lhat_nil(), at);
                            }
                            from = resolve_ordinal(from, subject->characters);
                            if (from < 1) {
                                from = 1;
                            }
                            size_t start_byte =
                                (size_t)from - 1 <= subject->characters
                                    ? lhat_string_byte_at(subject,
                                                          (size_t)from - 1)
                                    : subject->length;
                            size_t found = 0;
                            if (!find_bytes(subject->text, subject->length,
                                            start_byte, needle->text,
                                            needle->length, &found)) {
                                SET_R(a, lhat_nil());
                                break;
                            }
                            SET_R(a, lhat_integer(
                                          (int64_t)characters_before(
                                              subject->text, found) +
                                          1));
                            break;
                        }

                        if (native->kind == LHAT_NATIVE_FINDALL) {
                            if (b != 1) {
                                return finish(m, chunk, LHAT_RUN_ARITY,
                                              lhat_nil(), at);
                            }
                            FindWalk *walk = (FindWalk *)lhat_alloc(
                                sizeof *walk + needle->length);
                            if (walk == NULL) {
                                return finish(m, chunk,
                                              LHAT_RUN_OUT_OF_MEMORY,
                                              lhat_nil(), at);
                            }
                            walk->subject = subject;
                            walk->next_byte = 0;
                            walk->chars_before = 0;
                            walk->needle_length = needle->length;
                            memcpy(walk->needle, needle->text,
                                   needle->length);
                            LhatCoroutine *made = lhat_host_iterator(
                                &m->objects, findall_step, walk,
                                findall_release, native->bound);
                            if (made == NULL) {
                                lhat_free(walk);
                                return finish(m, chunk,
                                              LHAT_RUN_OUT_OF_MEMORY,
                                              lhat_nil(), at);
                            }
                            SET_R(a, lhat_object((LhatObject *)made));
                            break;
                        }

                        // REPLACE: every stand, non-overlapping, no
                        // callback -- that vocabulary is std.regex's.
                        if (b != 2 ||
                            !lhat_is_object_kind(R(first + 1),
                                                 LHAT_OBJECT_STRING)) {
                            return finish(m, chunk,
                                          b != 2 ? LHAT_RUN_ARITY
                                                 : LHAT_RUN_TYPE_ERROR,
                                          lhat_nil(), at);
                        }
                        const LhatString *with =
                            (const LhatString *)lhat_as_object(R(first + 1));
                        if (needle->length == 0) {
                            SET_R(a, native->bound);  // nowhere to stand
                            break;
                        }
                        size_t stands = 0;
                        for (size_t i = 0;
                             find_bytes(subject->text, subject->length, i,
                                        needle->text, needle->length, &i);
                             i += needle->length) {
                            stands++;
                        }
                        if (stands == 0) {
                            SET_R(a, native->bound);
                            break;
                        }
                        size_t total = subject->length +
                                       stands * with->length -
                                       stands * needle->length;
                        char *text = (char *)lhat_alloc(total + 1);
                        if (text == NULL) {
                            return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                          lhat_nil(), at);
                        }
                        size_t out_at = 0;
                        size_t in_at = 0;
                        size_t found = 0;
                        while (find_bytes(subject->text, subject->length,
                                          in_at, needle->text,
                                          needle->length, &found)) {
                            memcpy(text + out_at, subject->text + in_at,
                                   found - in_at);
                            out_at += found - in_at;
                            memcpy(text + out_at, with->text, with->length);
                            out_at += with->length;
                            in_at = found + needle->length;
                        }
                        memcpy(text + out_at, subject->text + in_at,
                               subject->length - in_at);
                        out_at += subject->length - in_at;
                        LhatString *swapped =
                            lhat_string_new(&m->objects, text, out_at);
                        lhat_free(text);
                        if (swapped == NULL) {
                            return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                          lhat_nil(), at);
                        }
                        SET_R(a, lhat_object((LhatObject *)swapped));
                        break;
                    }

                    // 16.3: what `in^` walks. A table answers with a walk of
                    // its keys; a coroutine is already one.
                    // 16.3改2: the projections are made the same way and at
                    // the same moment -- one call, one walk -- which is what
                    // the parentheses are saying.
                    if (native->kind == LHAT_NATIVE_ITERATE ||
                        native->kind == LHAT_NATIVE_KEYS ||
                        native->kind == LHAT_NATIVE_VALUES) {
                        if (native->kind == LHAT_NATIVE_ITERATE &&
                            lhat_is_object_kind(native->bound,
                                                LHAT_OBJECT_COROUTINE)) {
                            SET_R(a, native->bound);
                            break;
                        }
                        const LhatTable *over = table_of(native->bound);
                        if (over == NULL) {
                            return finish(m, chunk, LHAT_RUN_NOT_CALLABLE, lhat_nil(),
                                          at);
                        }
                        LhatWalkPart part =
                            native->kind == LHAT_NATIVE_KEYS ? LHAT_WALK_KEYS
                            : native->kind == LHAT_NATIVE_VALUES
                                ? LHAT_WALK_VALUES
                                : LHAT_WALK_PAIR;
                        LhatCoroutine *walk =
                            lhat_table_iterator(&m->objects, over, part);
                        if (walk == NULL) {
                            return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                          at);
                        }
                        SET_R(a, lhat_object((LhatObject *)walk));
                        break;
                    }

                    if (!lhat_is_object_kind(native->bound,
                                             LHAT_OBJECT_COROUTINE)) {
                        return finish(m, chunk, LHAT_RUN_NOT_CALLABLE, lhat_nil(), at);
                    }
                    LhatCoroutine *co =
                        (LhatCoroutine *)lhat_as_object(native->bound);

                    // 15.6改: the two questions, answered before any of the
                    // guards below. Neither runs the body, so both hold on a
                    // finished coroutine, where everything else faults, and
                    // on a fresh one, where resume does.
                    if (native->kind == LHAT_NATIVE_DONE ||
                        native->kind == LHAT_NATIVE_STARTED) {
                        if (b != 0) {
                            return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(), at);
                        }
                        SET_R(a, lhat_bool(
                            native->kind == LHAT_NATIVE_DONE
                                ? co->state == LHAT_COROUTINE_DONE
                                : co->state != LHAT_COROUTINE_FRESH));
                        break;
                    }

                    // 15.2: the machine holds this itself rather than
                    // trusting the checker to have (vm.h's opening comment).
                    // start takes none, since nothing has been yield^ed yet to
                    // send a value to. 13.9: a resume takes one where R is
                    // there and none where it is empty -- a body no var^ of
                    // which receives a yield^ has nothing being sent in.
                    // 16.3's built-in walk is one of those; nothing sends it
                    // anything either.
                    if (native->kind == LHAT_NATIVE_START && b != 0) {
                        return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(), at);
                    }
                    if (native->kind == LHAT_NATIVE_RESUME) {
                        const LhatProto *from =
                            co->source == LHAT_COROUTINE_BODY &&
                                    co->closure != NULL
                                ? co->closure->proto
                                : NULL;
                        // 16.3's built-in walk receives nothing either, and
                        // has no proto to say so. 13.8改: a tuple R takes
                        // that many arguments -- but only a checked body has
                        // reserved the run's slots for the yield^'s binding,
                        // so an unknown proto keeps the old ceiling of one:
                        // a run laid where nothing reserved it would write
                        // over the frame's own locals.
                        bool known = from == NULL || from->yield_receives_known;
                        uint8_t wants =
                            from != NULL ? from->yield_receive_count : 0;
                        if (known ? b != wants : b > 1) {
                            return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(),
                                          at);
                        }
                        // And the run still has to fit the suspended frame
                        // (5.1's stop, never a scribble past it).
                        if (b > 1 && co->state == LHAT_COROUTINE_SUSPENDED &&
                            (size_t)co->sent_into + b >= co->register_count) {
                            return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(),
                                          at);
                        }
                    }

                    // 15.2: start and resume split the two jobs, so each has
                    // to be called on the state that makes it meaningful.
                    if (native->kind == LHAT_NATIVE_START &&
                        co->state != LHAT_COROUTINE_FRESH) {
                        return finish(m, chunk, LHAT_RUN_COROUTINE_ALREADY_STARTED,
                                      lhat_nil(), at);
                    }
                    if (native->kind == LHAT_NATIVE_RESUME &&
                        co->state == LHAT_COROUTINE_FRESH) {
                        return finish(m, chunk, LHAT_RUN_COROUTINE_NOT_STARTED,
                                      lhat_nil(), at);
                    }

                    // 02 の 10.7: disposal runs what is still pending and
                    // never runs the same cleanup twice, so a coroutine that
                    // has finished simply has nothing left to do.
                    bool dispose = native->kind == LHAT_NATIVE_DISPOSE;
                    if (co->state == LHAT_COROUTINE_DONE ||
                        (dispose && co->state == LHAT_COROUTINE_FRESH)) {
                        if (dispose) {
                            co->state = LHAT_COROUTINE_DONE;
                            // 05 の 8.8: a host walk's state goes back with
                            // the disposal. Nothing on any other source.
                            lhat_coroutine_release((LhatObject *)co, m);
                            SET_R(a, lhat_nil());
                            break;
                        }
                        return finish(m, chunk, LHAT_RUN_DEAD_COROUTINE, lhat_nil(), at);
                    }
                    if (co->state == LHAT_COROUTINE_RUNNING) {
                        return finish(m, chunk, LHAT_RUN_DEAD_COROUTINE, lhat_nil(), at);
                    }

                    // 16.3: a walk of a table has no body, so nothing below
                    // applies to it -- no frame to enter and nothing pending
                    // to drain. The guards above leave start() on a fresh
                    // walk and resume() on a suspended one, and one step is
                    // the whole of either.
                    if (co->source == LHAT_COROUTINE_TABLE) {
                        if (dispose) {
                            co->state = LHAT_COROUTINE_DONE;
                            SET_R(a, lhat_nil());
                            break;
                        }
                        unsigned room = lhat_call_prepared(cc);
                        // 16.3改2: a projection yields one value, so one slot
                        // is what it wants and a run has nothing to put in
                        // the second position.
                        if (co->part != LHAT_WALK_PAIR) {
                            if (room > 1) {
                                return finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                              lhat_nil(), at);
                            }
                            step_table_walk(m, co, WALK_AS_VALUE, rbase + a,
                                            frame);
                            break;
                        }
                        // 16.3 with 13.8改: hand-driven start()/resume()
                        // answer the pair as the tuple every walk yields.
                        // The call's reservation says whether the run has
                        // anywhere to land; a checked call reserved the
                        // width (the stamp on a discarded one included),
                        // and a mismatch is refused the way 03 の 5.3
                        // refuses any tuple answer.
                        if (room > 1 && room != 3) {
                            return finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                          lhat_nil(), at);
                        }
                        if (room == 3) {
                            if (rbase + a + 2 >= LHAT_STACK_SLOTS) {
                                return finish(m, chunk,
                                              LHAT_RUN_STACK_OVERFLOW,
                                              lhat_nil(), at);
                            }
                            step_table_walk(m, co, WALK_AS_RUN, rbase + a,
                                            frame);
                            break;
                        }
                        // One slot reserved: the step still advances, and
                        // the walk ending still answers nil^; a pair coming
                        // back has nowhere to land.
                        LhatValue key, value;
                        if (!lhat_table_walk(co, &key, &value)) {
                            co->state = LHAT_COROUTINE_DONE;
                            SET_R(a, lhat_nil());
                            break;
                        }
                        co->state = LHAT_COROUTINE_SUSPENDED;
                        return finish(m, chunk, LHAT_RUN_TUPLE_UNEXPECTED,
                                      lhat_nil(), at);
                    }

                    // 05 の 8.8: a host's walk is the same shape -- no body,
                    // no frame, one step per call. The step itself says what
                    // shape it yields, so the reservation is judged inside
                    // rather than up front the way a table's part allows.
                    if (co->source == LHAT_COROUTINE_HOST) {
                        if (dispose) {
                            co->state = LHAT_COROUTINE_DONE;
                            lhat_coroutine_release((LhatObject *)co, m);
                            SET_R(a, lhat_nil());
                            break;
                        }
                        unsigned room = lhat_call_prepared(cc);
                        if (room > 1 &&
                            rbase + a + room - 1 >= LHAT_STACK_SLOTS) {
                            return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW,
                                          lhat_nil(), at);
                        }
                        LhatRunStatus fault = LHAT_RUN_OK;
                        // 13.8改: everything the resume sent, not the
                        // first of it. The body path below gathers the
                        // same way; a walk is no less entitled to it.
                        LhatValue walk_sent[LHAT_MAX_TUPLE];
                        size_t walk_sent_count = 0;
                        for (size_t i = 0;
                             i < (size_t)b && i < LHAT_MAX_TUPLE; i++) {
                            walk_sent[walk_sent_count++] = R(first + i);
                        }
                        step_host_walk(m, co,
                                       room > 1 ? WALK_AS_RUN : WALK_AS_VALUE,
                                       rbase + a,
                                       room > 1 ? (size_t)room - 1
                                                : (size_t)room,
                                       frame, walk_sent, walk_sent_count,
                                       &fault);
                        if (fault != LHAT_RUN_OK) {
                            return finish(m, chunk, fault, lhat_nil(), at);
                        }
                        break;
                    }

                    if (m->frame_count >= LHAT_MAX_FRAMES) {
                        return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                    }

                    size_t next_base = rbase + (a) + 1;
                    if (next_base + LHAT_MAX_REGISTERS >= LHAT_STACK_SLOTS) {
                        return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                    }
                    frame->pc = pc;

                    if (dispose) {
                        // 10.7: what is pending runs, innermost first, and
                        // then the coroutine is finished.
                        enter_disposal_frame(m, co, next_base, a, &frame,
                                                                  &rbase, &chunk, &pc);
                        // 15.4: the guards above leave only a suspended
                        // coroutine here, so the slot a yield^ answers into
                        // is written the way a resume writes it -- with
                        // nil^, since dispose sends nothing in.
                        SET_R(co->sent_into, sent);
                        goto drain;
                    }

                    // 5.11: one frame, put back where it left off. 13.8改:
                    // `prepared` carries what the call site reserved, so the
                    // callee can tell whether a run is wanted here -- and
                    // the resume's arguments go over as they were written,
                    // however many the R takes.
                    LhatValue sent_run[LHAT_MAX_TUPLE];
                    size_t sent_count = 0;
                    for (size_t i = 0; i < (size_t)b && i < LHAT_MAX_TUPLE;
                         i++) {
                        sent_run[sent_count++] = R(first + i);
                    }
                    // 05 の 8.9: a host value argument goes over whole, as
                    // the pointer form, and rides alone -- a run's positions
                    // are single slots, so it mixes with nothing.
                    if (sent_count == 1 && lhat_is_hostvalue(sent_run[0])) {
                        sent_run[0] =
                            stash_sent_hostvalue(m, rbase + first);
                        const LhatHostValueTag *sent_tag =
                            sent_run[0].as.hostvalue_run[0].hostvalue;
                        if ((size_t)co->sent_into + sent_tag->width >
                            co->register_count) {
                            return finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                          lhat_nil(), at);
                        }
                    } else {
                        for (size_t i = 0; i < sent_count; i++) {
                            if (lhat_is_hostvalue(sent_run[i])) {
                                return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                              lhat_nil(), at);
                            }
                        }
                    }
                    frame = enter_resume_frame(
                        m, co, next_base, a, (uint8_t)lhat_call_prepared(cc),
                        sent_run, sent_count);
                    rbase = frame->base;
                    chunk = &co->closure->proto->chunk;
                    pc = frame->pc;
                    break;
                }

                if (!lhat_is_object_kind(R(a), LHAT_OBJECT_SUBROUTINE)) {
                    return finish(m, chunk, LHAT_RUN_NOT_CALLABLE, lhat_nil(), at);
                }
                const LhatClosure *callee =
                    (const LhatClosure *)lhat_as_object(R(a));
                if (callee->proto == NULL) {
                    return finish(m, chunk, LHAT_RUN_NOT_CALLABLE, lhat_nil(), at);
                }

                // 14.4: the receiver sits between the callee and the
                // arguments, and whether it is passed depends on the callee.
                // A member that takes no self^ is a static one (14.4), so the
                // frame simply starts after the receiver.
                size_t given = b;
                size_t skip = 1;
                if (as_method) {
                    if (callee->proto->takes_self) {
                        given = (size_t)b + 1;
                    } else {
                        skip = 2;
                    }
                }
                // 13.7: 'expr...' put a table in the last slot instead of an
                // ordinary argument; its positions are unpacked in place of
                // it, which is why what a call owes is read through
                // call_arg() from here on rather than off registers directly.
                const LhatTable *spread_table = NULL;
                size_t spread_run = SIZE_MAX;
                size_t before_spread = given;
                if ((cc & LHAT_CALL_SPREAD) != 0) {  // 13.8改, as above
                    if (given == 0) {
                        return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                    }
                    LhatValue spread_from = R(a + skip + given - 1);
                    before_spread = given - 1;
                    // 13.8改: a tuple spreads as the run it already is -- the
                    // positions are the slots past the head, so there is
                    // nothing to unpack and nothing to allocate.
                    if (lhat_is_run(spread_from)) {
                        spread_run = rbase + a + skip + given - 1;
                        given = before_spread + lhat_run_width(spread_from);
                    } else {
                        spread_table = table_of(spread_from);
                        if (spread_table == NULL) {
                            return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                          lhat_nil(), at);
                        }
                        given = before_spread + spread_table->array_count;
                    }
                }
                // 13.7: the last slot of a variadic callee collects the rest
                // into a table rather than taking one argument for itself, so
                // a call owes at least the fixed count and not exactly the
                // slot count.
                size_t declared_slots = callee->proto->parameters;
                size_t required = callee->proto->has_variadic
                                      ? declared_slots - 1
                                      : declared_slots;
                // 13.4: nothing fills a missing argument in. A parameter's
                // default belongs to the editor that writes the call, so the
                // count owed here is the declared one either way.
                if (callee->proto->has_variadic ? given < required
                                                : given != declared_slots) {
                    return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(), at);
                }
                // Built before either path below places it, since both read
                // the same arguments the same way.
                LhatValue collected_variadic = lhat_nil();
                if (callee->proto->has_variadic) {
                    LhatTable *collected = lhat_table_new(&m->objects);
                    if (collected == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(),
                                      at);
                    }
                    for (size_t i = required; i < given; i++) {
                        bool refused = false;
                        LhatValue value = call_arg(m->slots, rbase, a, skip,
                                                   spread_table, spread_run,
                                                   before_spread,
                                                   i);
                        // 05 の 8.9: the collection is a table, and a table
                        // member is never a host value -- the checker said
                        // so at the call; this is the unchecked backstop.
                        if (lhat_is_hostvalue(value)) {
                            return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                          lhat_nil(), at);
                        }
                        if (!set_key(m, collected,
                                     lhat_integer((int64_t)(i - required + 1)),
                                     value, &refused)) {
                            return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                          lhat_nil(), at);
                        }
                    }
                    collected_variadic = lhat_object((LhatObject *)collected);
                }

                // 02 の 15.5: calling a yieldable procedure does not suspend
                // the caller. It answers a coroutine, and the body has not
                // started -- which is why the colouring of async/await never
                // arises here.
                if (callee->proto->yields) {
                    LhatCoroutine *co =
                        lhat_coroutine_new(&m->objects, callee,
                                           callee->proto->chunk.registers);
                    if (co == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
                    }
                    // 05 の 8.9: without a spread the arguments sit in their
                    // laid-out slots, widths included, so the copy is
                    // slot-blind -- what lets a wide parameter cross. A
                    // spread re-indexes by value, and never mixes with a
                    // wide argument (the compiler refused that pair, and
                    // wide with variadic likewise).
                    if (spread_table == NULL && spread_run == SIZE_MAX &&
                        !callee->proto->has_variadic) {
                        for (size_t i = 0; i < callee->proto->parameter_slots;
                             i++) {
                            lhat_slots_set(
                                co->registers, i,
                                lhat_slots_get(m->slots,
                                               rbase + a + skip + i));
                        }
                    } else {
                        size_t fixed =
                            callee->proto->has_variadic ? required : given;
                        for (size_t i = 0; i < fixed; i++) {
                            lhat_slots_set(co->registers, i,
                                           call_arg(m->slots, rbase, a, skip,
                                                    spread_table, spread_run,
                                                    before_spread, i));
                        }
                    }
                    if (callee->proto->has_variadic) {
                        lhat_slots_set(co->registers, required, collected_variadic);
                    }
                    SET_R(a, lhat_object((LhatObject *)co));
                    break;
                }

                // 5.3: a tail call runs in this frame rather than one above
                // it, where the frame is free to go. It is not free while a
                // cleanup is pending (5.5 runs those after the call, and a
                // frame that has left cannot run them), nor when it is a
                // coroutine's (5.11 -- the frame is that coroutine's body),
                // nor at the top level of a session (03 の 4.3 keeps those
                // slots for the next input).
                bool reuse = tail && frame->cleanup_count == 0 &&
                             frame->coroutine == NULL &&
                             frame->closure->proto->kept == 0;
                if (!reuse && m->frame_count >= LHAT_MAX_FRAMES) {
                    return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                }

                // 5.3: the arguments already sit just above the callee, so
                // the new frame starts there and needs no shuffling -- unless
                // a spread broke the contiguity, or 13.7's collector has to
                // overwrite the slot after the fixed ones with the table just
                // built.
                size_t next_base = rbase + a + skip;
                if (next_base + LHAT_MAX_REGISTERS >= LHAT_STACK_SLOTS) {
                    return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                }
                if (spread_table != NULL) {
                    size_t fixed = callee->proto->has_variadic ? required
                                                               : given;
                    for (size_t i = 0; i < fixed; i++) {
                        lhat_slots_set(m->slots, next_base + i,
                                       call_arg(m->slots, rbase, a, skip,
                                                spread_table, spread_run,
                                                before_spread, i));
                    }
                }
                if (callee->proto->has_variadic) {
                    lhat_slots_set(m->slots, next_base + (required), collected_variadic);
                }
                clear_scratch(m, next_base, callee->proto);

                // 5.3: the window the callee is about to run in has been laid
                // out above this frame; taking the frame over is moving it
                // down onto this frame's own registers. Nothing above is read
                // again, and next_base is always higher than base (a callee
                // sits at least one slot above the frame it was called from),
                // so the move is forward and needs no care about overlap.
                if (reuse) {
                    // 5.4: what still points into these registers takes its
                    // value with it first. Closing leaves the slots as they
                    // are, so the arguments being moved down are still there
                    // to read.
                    close_upvalues(m, frame->base);
                    size_t window = callee->proto->chunk.registers;
                    for (size_t i = 0; i < window; i++) {
                        // 05 の 8.9: a host value's continuation slots are raw
                        // bytes, so the payload and the tag travel exactly as
                        // they lie rather than through a value read.
                        m->slots.values[frame->base + i] =
                            m->slots.values[next_base + i];
                        m->slots.tags[frame->base + i] =
                            m->slots.tags[next_base + i];
                    }
                    frame->closure = callee;
                    frame->pc = 0;
                    frame->returning = false;
                    // 5.3: `base`, `result` and `prepared` are the original
                    // caller's and stay its own -- the answer still goes where
                    // that call site reserved room for it. 11.9's `derive`
                    // stays for the same reason: what this frame answers is
                    // still read the way the expression that made it asks.
                    // The drop is sticky: a body whose answer was already
                    // being thrown away throws away whatever it goes on to
                    // tail-call for.
                    if ((cc & LHAT_CALL_DROP) != 0) {
                        frame->drop_answer = true;
                    }
                    chunk = &callee->proto->chunk;
                    pc = 0;
                    break;
                }

                frame->pc = pc;
                Frame *called = &m->frames[m->frame_count++];
                called->closure = callee;
                called->pc = 0;
                called->base = next_base;
                called->result = a;
                called->prepared = (uint8_t)lhat_call_prepared(cc);  // 13.8改
                called->cleanup_count = 0;  // 5.5: pending cleanups are per frame
                called->returning = false;
                called->coroutine = NULL;
                called->disposing = false;
                called->derive = LHAT_FRAME_NO_DERIVE;
                called->derive_equal = false;
                called->drop_answer = false;  // 5.3
                // The room is a root while the frame lives (mark_roots), so
                // it starts empty rather than as whatever the slot held
                // before.
                called->answer = lhat_nil();

                frame = called;
                rbase = frame->base;
                chunk = &callee->proto->chunk;
                pc = 0;
                break;
            }

            case LHAT_BC_PUSHCLEANUP:
                if (frame->cleanup_count >= LHAT_MAX_CLEANUPS) {
                    return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                }
                frame->cleanups[frame->cleanup_count++] = lhat_bx(instruction);
                break;

            // 10.2 and 12.3: leaving the block runs what it entered. The two
            // cases differ only in where control goes afterwards.
            case LHAT_BC_POPCLEANUP:
                frame->drain_target = a;
                frame->resume = pc;
                frame->returning = false;
                goto drain;

            case LHAT_BC_ENDCLEANUP:
                goto drain;

            // 10.4: leaving the procedure leaves every block inside it, so
            // the drain runs everything still pending before the frame goes.
            case LHAT_BC_RETURN:
            case LHAT_BC_RETURN_NIL:
                frame->drain_target = 0;
                frame->returning = true;
                frame->answer = op == LHAT_BC_RETURN ? R(a) : lhat_nil();
                // 05 の 8.9: a host value answer moves into the frame's own
                // room before the drain -- the callee's window overlaps the
                // caller's scratch, so no register survives the cleanups.
                // The pop places it whole from there. A coroutine's own
                // return crossing a suspension was refused by the checker;
                // this is the backstop.
                if (op == LHAT_BC_RETURN && lhat_is_hostvalue(R(a))) {
                    // 8.9改2: the base is no exception any more -- the drain
                    // hands the value to the host whole, off this same room.
                    const LhatHostValueTag *tag = lhat_as_hostvalue_tag(R(a));
                    for (size_t i = 0; i < tag->width; i++) {
                        frame->answer_run[i] = m->slots.values[rbase + a + i];
                    }
                    frame->answer.as.hostvalue_run = frame->answer_run;
                }
                // 02 の 13.8改: a tuple answer moves into the same room, for
                // the same reason -- no register survives the drain. The
                // positions are values rather than raw bytes, so their tags
                // travel beside them, and the collector reads them there
                // (gc.c's mark_roots) while the cleanups run.
                if (op == LHAT_BC_RETURN && b != 0) {
                    if ((size_t)b > LHAT_MAX_TUPLE) {
                        return finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                      lhat_nil(), at);
                    }
                    for (size_t i = 0; i < (size_t)b; i++) {
                        frame->answer_run[i + 1] =
                            m->slots.values[rbase + a + i];
                        frame->answer_tags[i + 1] =
                            m->slots.tags[rbase + a + i];
                    }
                    frame->answer = lhat_run_head((size_t)b);
                }
                goto drain;

            // 04 の 11.6: unlike a fault the machine itself raises, the
            // value is the program's own -- carried through to the host
            // exactly as lhat_run always has, rather than discarded like
            // every other finish() here discards its nil^.
            case LHAT_BC_PANIC:
                return finish(m, chunk, LHAT_RUN_PANIC, R(a), at);

            // 11.6: 14.12's own runtime check (fits_call already
            // trusts it for overload^ resolution), just asked once instead
            // of per candidate. Compile-time disjointness (check.c) already
            // ruled out what could never hold; this is what it could not
            // rule out, checked against the actual value.
            // 02 の 11.6改3: where the value fits, it stays and only the
            // type the checker tracks narrowed. Where it does not, the
            // answer is the other arm of what as^ was said to be -- a
            // localerror^.CastFailure, which 04 の 2.7改 will not let the
            // frame return, so a writer meets it here or nowhere.
            //
            // 03 の 4.2: this runs whether or not anything was checked. A
            // relaxed build makes the same value the strict one promised.
            case LHAT_BC_ASCAST: {
                const LhatRuntimeType *wanted =
                    (const LhatRuntimeType *)lhat_as_object(R(b));
                if (!lhat_value_satisfies(R(a), wanted)) {
                    const LhatErrorKind *kind = lhat_registry_cast_failure();
                    LhatError *failure =
                        kind != NULL ? lhat_error_new(&m->objects, kind) : NULL;
                    if (failure == NULL) {
                        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY,
                                      lhat_nil(), at);
                    }
                    SET_R(a, lhat_object((LhatObject *)failure));
                }
                break;
            }

            // 02 の 15.4: the frame stops here and the value goes out. 5.11
            // keeps the one frame rather than a stack, which 15.5 is what
            // makes possible -- a yield^ is always in the body it suspends.
            case LHAT_BC_YIELD: {
                LhatCoroutine *co = frame->coroutine;
                // 10.7: nothing is waiting for a yield^ during disposal.
                if (co == NULL || frame->disposing) {
                    return finish(m, chunk, LHAT_RUN_YIELD_OUTSIDE, lhat_nil(), at);
                }
                LhatValue value = R(a);
                // 02 の 13.8改: a tuple goes out whole. The positions ride
                // the frame's own room the way a return^'s do -- the window
                // is about to be copied into the coroutine and then left
                // behind, so no register survives to be read from the
                // resumer's side. What the resume sends comes back into the
                // first position's slot (co->sent_into below) whatever its
                // own width is -- one value, a host value laid out whole, or
                // 13.8改's several as a run head with the positions after.
                //
                // b == 0 with a run head in the slot is 15.8's delegation
                // loop forwarding what it was handed: the positions are
                // already in this frame's answer room, put there by the
                // RESUME just above -- by WALK_AS_ANSWER for a table's walk,
                // or by the inner body's own yield collating into it -- so
                // there is nothing to fill and the head goes out as it
                // stands.
                // 05 の 8.9: b == LHAT_YIELD_HOSTVALUE says R[A..] is a host
                // value laid out whole. It rides the frame's own room the
                // way a return's does -- the window is about to be left
                // behind -- and goes out as the pointer form the placement
                // reads.
                if (b == LHAT_YIELD_HOSTVALUE) {
                    if (!lhat_is_hostvalue(value)) {
                        return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                      lhat_nil(), at);
                    }
                    const LhatHostValueTag *tag = lhat_as_hostvalue_tag(value);
                    for (size_t i = 0; i < tag->width; i++) {
                        frame->answer_run[i] = m->slots.values[rbase + a + i];
                    }
                    value.as.hostvalue_run = frame->answer_run;
                } else if (b != 0) {
                    if ((size_t)b > LHAT_MAX_TUPLE) {
                        return finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                      lhat_nil(), at);
                    }
                    for (size_t i = 0; i < (size_t)b; i++) {
                        frame->answer_run[i + 1] =
                            m->slots.values[rbase + a + i];
                        frame->answer_tags[i + 1] =
                            m->slots.tags[rbase + a + i];
                    }
                    value = lhat_run_head((size_t)b);
                } else if (lhat_is_hostvalue(value)) {
                    // b == 0 forwards what a delegation was handed. A host
                    // value arrives re-aimed at this frame's own room (see
                    // the hand-back below), so anything aimed anywhere else
                    // was never one of ours -- refused before the tag is
                    // ever read through it.
                    if (value.as.hostvalue_run != frame->answer_run) {
                        return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                      lhat_nil(), at);
                    }
                }

                for (size_t i = 0; i < co->register_count; i++) {
                    lhat_slots_set(co->registers, i, R(i));
                    // 5.12: the coroutine has just taken a whole frame's
                    // worth of the stack into itself. The backward barrier,
                    // as for a table: one more visit to the coroutine costs
                    // less than marking every register as it is copied.
                    lhat_gc_barrier_back(m, (LhatObject *)co, R(i));
                }
                // 15.4 with 5.4: the captures of this frame's slots travel
                // with the registers, or a resume at another depth would
                // leave them reading whatever frame took these addresses
                // over. This frame is the top one, so its captures are the
                // head of the machine's descending open list; popped highest
                // first and prepended, the coroutine's own list comes out
                // ascending, which is what reattach_upvalues expects.
                const LhatValueUnion *suspending = m->slots.values + rbase;
                while (m->open != NULL &&
                       m->open->location.value >= suspending) {
                    LhatUpvalue *up = m->open;
                    m->open = up->next_open;
                    size_t offset = (size_t)(up->location.value - suspending);
                    up->location = lhat_slots_ref(co->registers, offset);
                    up->suspended_in = co;
                    up->next_open = co->open;
                    co->open = up;
                    // 5.12: the capture now keeps the coroutine alive
                    // (gc.c), which a black upvalue has to declare.
                    lhat_gc_barrier(m, (LhatObject *)up,
                                    lhat_object((LhatObject *)co));
                }
                co->pc = pc;
                co->sent_into = a;
                co->state = LHAT_COROUTINE_SUSPENDED;
                if (frame->cleanup_count > LHAT_COROUTINE_CLEANUPS) {
                    return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                }
                co->cleanup_count = frame->cleanup_count;
                for (size_t i = 0; i < frame->cleanup_count; i++) {
                    co->cleanups[i] = frame->cleanups[i];
                }

                uint8_t into = frame->result;
                // 13.8改: read before `frame` becomes the resumer below. The
                // room outlives the pop -- the entry is still there, only
                // uncounted.
                uint8_t reserved = frame->prepared;
                const LhatValueUnion *out_run = frame->answer_run;
                const uint8_t *out_tags = frame->answer_tags;
                // 05 の 8.8: no resumer below -- the host resumed this
                // coroutine at the base of this run (lhat_machine_resume),
                // so the yield leaves run_frames the way a return does, the
                // coroutine staying suspended for the next resume. The
                // answer crosses the boundary the way a return's does: one
                // value, or the positions copied into the machine's room.
                if (m->frame_count == base_depth + 1) {
                    m->frame_count--;
                    // 05 の 8.9改2: the host receives the value whole, as
                    // the pointer form aimed at the machine's scratch.
                    if (lhat_is_hostvalue(value)) {
                        return finish(m, chunk, LHAT_RUN_OK,
                                      hand_hostvalue_out(m, value), at);
                    }
                    if (lhat_is_run(value)) {
                        size_t positions = lhat_run_width(value);
                        if (positions > LHAT_MAX_TUPLE) {
                            return finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                          lhat_nil(), at);
                        }
                        for (size_t i = 0; i < positions; i++) {
                            LhatValue held;
                            held.as = out_run[i + 1];
                            held.tag = (LhatValueTag)out_tags[i + 1];
                            m->tuple_scratch[i] = held;
                        }
                        m->tuple_scratch_count = positions;
                        value =
                            positions > 0 ? m->tuple_scratch[0] : lhat_nil();
                    }
                    return finish(m, chunk, LHAT_RUN_OK, value, at);
                }
                m->frame_count--;
                frame = &m->frames[m->frame_count - 1];
                rbase = frame->base;
                chunk = &frame->closure->proto->chunk;
                pc = frame->pc;
                if (lhat_is_hostvalue(value) && reserved == 0) {
                    // 15.8: the delegation loop forwards the value. The
                    // bytes move into the resumer's own room -- the popped
                    // frame's entry would be taken over by the next push --
                    // and the head is re-aimed there, keeping the invariant
                    // the YIELD above checks.
                    const LhatValueUnion *from = value.as.hostvalue_run;
                    const LhatHostValueTag *tag = from[0].hostvalue;
                    for (size_t i = 0; i < tag->width; i++) {
                        frame->answer_run[i] = from[i];
                    }
                    value.as.hostvalue_run = frame->answer_run;
                    lhat_slots_set(m->slots, rbase + into, value);
                } else if (lhat_is_hostvalue(value)) {
                    // 05 の 8.9: written out whole at the resume's slot, as
                    // a call's answer is at its own -- the call said the
                    // width it made room for (compile's prepared).
                    if ((size_t)reserved <
                            value.as.hostvalue_run[0].hostvalue->width ||
                        !place_hostvalue_answer(m, rbase + into, value)) {
                        return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                      lhat_nil(), at);
                    }
                } else if (lhat_is_run(value) && reserved == 0) {
                    // 15.8: the resumer is a delegation loop, which reserved
                    // one slot because it could not know this width (see
                    // RESUME). The head goes in that slot and the positions
                    // into the resumer's own answer room -- which is exactly
                    // where the YIELD it runs next reads a run's positions
                    // from, so the run is forwarded outwards untouched, and
                    // a chain of delegations hands it along one room at a
                    // time.
                    //
                    // The positions are safe across the collections the
                    // instructions between here and that YIELD may take: the
                    // loop holds the coroutine in a register for as long as
                    // it drives it, the yield above has just copied its
                    // registers into it, and everything yielded came from
                    // there. Reachability is through the coroutine, not
                    // through this room -- gc.c only walks the room for a
                    // frame whose answer is a run, which is a return in
                    // progress and not this.
                    size_t positions = lhat_run_width(value);
                    for (size_t i = 0; i < positions; i++) {
                        frame->answer_run[i + 1] = out_run[i + 1];
                        frame->answer_tags[i + 1] = out_tags[i + 1];
                    }
                    lhat_slots_set(m->slots, rbase + into, value);
                } else if (lhat_is_run(value)) {
                    // 13.8改: laid out as a head slot naming the width plus
                    // the positions, exactly as a returned tuple is. 13.9's
                    // 'union(Y, T)' is what the resumer holds, and the head's
                    // tag is what tells the two apart there. A narrower run
                    // (the short side of a widened fold) lands as it is --
                    // CHECKRUN pads it where the binding takes it apart.
                    size_t positions = lhat_run_width(value);
                    if ((size_t)reserved < positions + 1 ||
                        rbase + into + positions >= LHAT_STACK_SLOTS) {
                        return finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                      lhat_nil(), at);
                    }
                    lhat_slots_set(m->slots, rbase + into, value);
                    for (size_t i = 0; i < positions; i++) {
                        LhatValue held;
                        held.as = out_run[i + 1];
                        held.tag = (LhatValueTag)out_tags[i + 1];
                        lhat_slots_set(m->slots, rbase + into + 1 + i, held);
                    }
                } else if (reserved > 1 &&
                           lhat_is_object_kind(value, LHAT_OBJECT_TABLE)) {
                    // 16.3 with 13.8改: a loop reserved a run and the body
                    // yielded a table -- the iterator that answers pairs as
                    // tables rather than tuples. Its positions go into the
                    // reserved slots -- indexed here rather than by the loop --
                    // so the two iterator shapes meet the same binds. A missing position is 04 の 11.3's
                    // absence, which a destructuring faults on.
                    const LhatTable *yielded =
                        (const LhatTable *)lhat_as_object(value);
                    size_t positions = (size_t)reserved - 1;
                    if (rbase + into + positions >= LHAT_STACK_SLOTS) {
                        return finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                      lhat_nil(), at);
                    }
                    for (size_t i = 0; i < positions; i++) {
                        LhatValue held = lhat_table_get(
                            yielded, lhat_integer((int64_t)i + 1));
                        if (lhat_is_nil(held)) {
                            return finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                          lhat_nil(), at);
                        }
                        lhat_slots_set(m->slots, rbase + into + 1 + i, held);
                    }
                    lhat_slots_set(m->slots, rbase + into,
                                   lhat_run_head(positions));
                } else {
                    SET_R(into, value);
                }
                break;
            }

            // 02 の 15.8: the delegation loop asks whether the inner one is
            // finished, since 13.9 makes what a resume answers the union of
            // its yield type and its return type.
            case LHAT_BC_ISDONE: {
                if (!lhat_is_object_kind(R(b), LHAT_OBJECT_COROUTINE)) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                const LhatCoroutine *co =
                    (const LhatCoroutine *)lhat_as_object(R(b));
                SET_R(a, lhat_bool(co->state == LHAT_COROUTINE_DONE));
                break;
            }

            case LHAT_BC_RESUME: {
                if (!lhat_is_object_kind(R(b), LHAT_OBJECT_COROUTINE)) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
                }
                LhatCoroutine *co =
                    (LhatCoroutine *)lhat_as_object(R(b));
                if (co->state == LHAT_COROUTINE_DONE ||
                    co->state == LHAT_COROUTINE_RUNNING) {
                    return finish(m, chunk, LHAT_RUN_DEAD_COROUTINE, lhat_nil(), at);
                }

                // 16.3 with 13.8改: which shape one step puts down. C is the
                // loop's word -- the count of names is syntax, so unchecked
                // and checked compiles say the same thing (03 の 4.2).
                // 0 (and 1) is 15.8's delegation loop, the one emitter of a
                // RESUME with no width: the pair rides the answer room and
                // the YIELD after it forwards the run as it stands.
                WalkMode mode = (cc & LHAT_RESUME_WIDE) != 0 ? WALK_AS_VALUE
                                : cc >= 3                    ? WALK_AS_RUN
                                : cc == 2                    ? WALK_AS_VALUE
                                                             : WALK_AS_ANSWER;

                // 16.3: a table's walk has no body to enter, so resuming it
                // is one step and nothing more.
                if (co->source == LHAT_COROUTINE_TABLE) {
                    // A run is two positions; a loop that reserved another
                    // width asked for something the walk does not yield.
                    // 16.3改2: and a projection yields no pair at all, so a
                    // loop written with two names is asking the same way.
                    if (mode == WALK_AS_RUN &&
                        (co->part != LHAT_WALK_PAIR || (size_t)cc != 3 ||
                         rbase + a + 2 >= LHAT_STACK_SLOTS)) {
                        return finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                      lhat_nil(), at);
                    }
                    step_table_walk(m, co, mode, rbase + a, frame);
                    break;
                }
                // 05 の 8.8: and so is a host's -- one C call. The loops
                // send nothing in; the delegation loop forwards R(a), which
                // is what a BODY's resume writes into sent_into below.
                if (co->source == LHAT_COROUTINE_HOST) {
                    if (mode == WALK_AS_RUN &&
                        rbase + a + (size_t)cc - 1 >= LHAT_STACK_SLOTS) {
                        return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW,
                                      lhat_nil(), at);
                    }
                    LhatRunStatus fault = LHAT_RUN_OK;
                    // 15.8 with 13.8改: what the outer resume sent may
                    // itself be a run, laid at R(a) head-first by this
                    // frame's own suspension -- the body path below reads
                    // it the same way. A for^ loop sends nothing.
                    LhatValue walk_sent[LHAT_MAX_TUPLE];
                    size_t walk_sent_count = 0;
                    if (mode == WALK_AS_ANSWER) {
                        if (lhat_is_run(R(a))) {
                            size_t width = lhat_run_width(R(a));
                            if (width > LHAT_MAX_TUPLE ||
                                rbase + a + width >= LHAT_STACK_SLOTS) {
                                return finish(m, chunk,
                                              LHAT_RUN_TUPLE_ARITY,
                                              lhat_nil(), at);
                            }
                            for (size_t i = 0; i < width; i++) {
                                walk_sent[walk_sent_count++] = R(a + 1 + i);
                            }
                        } else {
                            walk_sent[walk_sent_count++] = R(a);
                        }
                    }
                    step_host_walk(m, co, mode, rbase + a,
                                   mode == WALK_AS_RUN
                                       ? (size_t)cc - 1
                                       : (cc & LHAT_RESUME_WIDE) != 0
                                             ? (size_t)(cc & ~LHAT_RESUME_WIDE)
                                             : 0,
                                   frame, walk_sent, walk_sent_count,
                                   &fault);
                    if (fault != LHAT_RUN_OK) {
                        return finish(m, chunk, fault, lhat_nil(), at);
                    }
                    break;
                }
                if (m->frame_count >= LHAT_MAX_FRAMES) {
                    return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                }
                // 13.8改: the frame goes above the slots the loop reserved
                // for the answer, or the callee's window would overlap the
                // run about to be written back into this one.
                size_t next_base =
                    rbase + (a) +
                    ((cc & LHAT_RESUME_WIDE) != 0
                         ? (size_t)(cc & ~LHAT_RESUME_WIDE)
                         : cc >= 3 ? (size_t)cc : 1);
                if (next_base + LHAT_MAX_REGISTERS >= LHAT_STACK_SLOTS) {
                    return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
                }

                // 15.8 with 13.8改: the delegation loop forwards what the
                // outer resume sent, which may itself be a run -- laid at
                // R(a) by this frame's own suspension, head plus positions.
                // A for^ loop sends nil^, which is never one.
                LhatValue sent_run[LHAT_MAX_TUPLE];
                size_t sent_count = 1;
                sent_run[0] = R(a);
                if (lhat_is_hostvalue(R(a))) {
                    // 05 の 8.9: the outer resume laid the value out whole
                    // at this slot; it goes on as the pointer form, its
                    // bytes stashed clear of the restore.
                    sent_run[0] = stash_sent_hostvalue(m, rbase + a);
                    const LhatHostValueTag *sent_tag =
                        sent_run[0].as.hostvalue_run[0].hostvalue;
                    if (co->state == LHAT_COROUTINE_SUSPENDED &&
                        (size_t)co->sent_into + sent_tag->width >
                            co->register_count) {
                        return finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                      lhat_nil(), at);
                    }
                } else if (lhat_is_run(R(a))) {
                    size_t width = lhat_run_width(R(a));
                    if (width > LHAT_MAX_TUPLE ||
                        rbase + a + width >= LHAT_STACK_SLOTS) {
                        return finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                      lhat_nil(), at);
                    }
                    for (size_t i = 0; i < width; i++) {
                        sent_run[i] = R(a + 1 + i);
                    }
                    sent_count = width;
                }
                // As at the natives' resume: the run has to fit the
                // suspended frame (5.1's stop for an unchecked proto).
                if (sent_count > 1 &&
                    co->state == LHAT_COROUTINE_SUSPENDED &&
                    (size_t)co->sent_into + sent_count >= co->register_count) {
                    return finish(m, chunk, LHAT_RUN_TUPLE_ARITY, lhat_nil(),
                                  at);
                }
                frame->pc = pc;
                // 13.8改: what the resume reserved for the answer -- a loop
                // driving a tuple-yielding body says its width here, and the
                // yield's placement reads it (15.4).
                //
                // 15.8: zero is the delegation loop, and means "whatever
                // width comes, forward it". It cannot say a width: unlike a
                // for^, whose count of names is syntax (03 の 4.2), a
                // await^ would have to read the inner body's type to know
                // one, and an unchecked compile has no type to read. So the
                // width is settled where it is known -- at the yield -- and
                // the run travels through this frame's answer room rather
                // than through slots it never reserved.
                frame = enter_resume_frame(
                    m, co, next_base, a,
                    (cc & LHAT_RESUME_WIDE) != 0
                        ? (uint8_t)(cc & ~LHAT_RESUME_WIDE)
                        : cc >= 3 ? cc : (cc == 0 ? 0 : 1),
                    sent_run, sent_count);
                rbase = frame->base;
                chunk = &co->closure->proto->chunk;
                pc = frame->pc;
                break;
            }

            case LHAT_BC_COUNT:
                return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
        }
        continue;

    // 02 の 11.1: an operator is a function the left operand carries, named
    // by 11.8 after the operator itself. The instructions above take their
    // own types directly and come here for everything else, which is why the
    // built-in cases pay nothing for this.
    call_operator: {
        // 11.8改: NEG is the one instruction arriving here with a single
        // operand. Everything below reads 'cc' only where a right operand
        // exists, so the unary path leaves that register untouched -- NEG
        // never wrote one.
        bool unary = op == LHAT_BC_NEG;
        uint8_t given = unary ? 0 : 1;
        LhatValue found = lhat_nil();
        OperatorLookup answer = OPERATOR_ABSENT;
        for (;;) {
            size_t length = 0;
            const char *name = operator_name(op, &length);
            // 14.4 makes an operator a method: the left operand is the
            // receiver and the right one the single argument.
            answer = operator_candidate(m, R(b), name, length, R(b),
                                        unary ? lhat_nil() : R(cc), given,
                                        false, &found);
            // 11.3改: the left carries nothing that takes this right
            // operand, so the right one is asked whether it was written as
            // the receiver instead. This is what lets a value join an
            // operation whose left operand is a built-in, which can carry no
            // answer for it.
            //
            // 11.8改: a unary operator has no other side to ask. Its one
            // operand is the receiver by the only reading there is.
            if (!unary &&
                (answer == OPERATOR_ABSENT || answer == OPERATOR_NO_CANDIDATE)) {
                LhatValue other = lhat_nil();
                OperatorLookup right = operator_candidate(
                    m, R(cc), name, length, R(cc), R(b), given, true, &other);
                if (right == OPERATOR_PICKED || right == OPERATOR_NO_MEMORY) {
                    found = other;
                    answer = right;
                }
            }
            // 11.9改: an equality was looked for as '=' first. A type that
            // wrote none may still have said how it orders, and 11.9 has
            // that refine equality too -- so the other name is asked before
            // the default is taken.
            if (answer != OPERATOR_PICKED && op == LHAT_BC_EQ &&
                derive_from != LHAT_FRAME_NO_DERIVE) {
                op = LHAT_BC_SPACESHIP;
                continue;
            }
            break;
        }
        // 11.9改: which of the two answered, and so how what comes back is
        // read -- an op^= answers the bool^ itself, a '<=>' a number^ to put
        // beside zero.
        bool answered_bool = op == LHAT_BC_EQ;
        if (answer == OPERATOR_NO_MEMORY) {
            return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), at);
        }
        // 11.9: equality is answered whether or not either was written --
        // 14.2 says what a table is the same as and 05 の 8.9 what a host
        // value is, and a type that says more only refines that. An ordering
        // has no such answer to fall back on and faults the way it always did.
        if (answer != OPERATOR_PICKED &&
            (derive_from == LHAT_BC_EQ || derive_from == LHAT_BC_NE)) {
            bool equal;
            if (lhat_is_hostvalue(R(b)) || lhat_is_hostvalue(R(cc))) {
                // 05 の 8.9: the bytes under the same tag. Reading the heads
                // alone would call two same-typed values equal whatever they
                // hold.
                equal = lhat_is_hostvalue(R(b)) &&
                        hostvalue_equal(m->slots, rbase + b, rbase + cc);
            } else {
                equal = lhat_value_equal(R(b), R(cc));
            }
            SET_R(a, lhat_bool(equal == (derive_from == LHAT_BC_EQ)));
            continue;  // the label sits in the loop, not in the switch
        }
        if (answer == OPERATOR_NO_CANDIDATE) {
            return finish(m, chunk, LHAT_RUN_NO_CANDIDATE, lhat_nil(), at);
        }
        if (answer != OPERATOR_PICKED) {
            return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
        }
        // 05 の 8.9: a host value's operator is C and answers on the spot --
        // no frame, no derive to carry: the receiver convention is 14.4's,
        // with each operand handed over as its head aimed into the stack.
        if (lhat_is_object_kind(found, LHAT_OBJECT_HOST)) {
            LhatHost *carried_host = (LhatHost *)lhat_as_object(found);
            LhatValue operands[2];
            operands[0] = lhat_is_hostvalue(R(b))
                              ? hostvalue_argument(m->slots, rbase + b)
                              : R(b);
            if (!unary) {
                operands[1] = lhat_is_hostvalue(R(cc))
                                  ? hostvalue_argument(m->slots, rbase + cc)
                                  : R(cc);
            }
            frame->pc = pc;  // 11.6改, as at a CALL
            size_t frames_before = m->frame_count;
            LhatValue answered = lhat_nil();
            if (!call_host_fn(m, carried_host->call,
                              carried_host->context, operands,
                              unary ? 1 : 2, &answered)) {
                return finish(m, chunk, LHAT_RUN_TUPLE_ARITY, lhat_nil(),
                              at);
            }
            LhatRunStatus left = LHAT_RUN_OK;
            LhatValue left_with = lhat_nil();
            if (host_faulted(m, frames_before, &left, &left_with)) {
                return finish(m, chunk, left, left_with, at);
            }
            if (derive_from != LHAT_FRAME_NO_DERIVE) {
                bool held = false;
                LhatRunStatus status = LHAT_RUN_OK;
                if (answered_bool) {
                    // 11.9改: an op^= answers the judgement itself. The shape
                    // rule asks it for a bool^, so anything else is a body
                    // that did not keep to its signature.
                    if (!lhat_is_bool(answered)) {
                        return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                      lhat_nil(), at);
                    }
                    held = lhat_as_bool(answered) ==
                           (derive_from == LHAT_BC_EQ);
                } else if (derive_from == LHAT_BC_EQ ||
                           derive_from == LHAT_BC_NE) {
                    // 11.9: what came back is read against zero.
                    held = lhat_value_equal(answered, lhat_integer(0)) ==
                           (derive_from == LHAT_BC_EQ);
                } else if (!ordering(derive_from, answered, lhat_integer(0),
                                     &held, &status)) {
                    return finish(m, chunk, status, lhat_nil(), at);
                }
                SET_R(a, lhat_bool(held));
                continue;
            }
            // 02 の 13.8改: an operator answers one value, whoever wrote it
            // -- 11.8's shape gives it no room for several, and the frame
            // pushed for an L^ one says so too (prepared = 1 below). A host
            // that answered a run has nowhere to put it here.
            if (lhat_is_run(answered)) {
                m->tuple_scratch_count = 0;
                return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
            }
            if (lhat_is_hostvalue(answered)) {
                if (!place_hostvalue_answer(m, rbase + a, answered)) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(),
                                  at);
                }
                continue;
            }
            SET_R(a, answered);
            continue;
        }
        const LhatClosure *carried =
            (const LhatClosure *)lhat_as_object(found);
        // 15.7改: an operator may not be yieldable. Not because it is an f^ --
        // 15.3改 lets one of those suspend -- but because 11.8's signature
        // answers T, and a yieldable subroutine answers a coroutine (15.5).
        if (carried->proto == NULL || carried->proto->yields) {
            return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), at);
        }
        if (m->frame_count >= LHAT_MAX_FRAMES) {
            return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
        }

        // 5.3 wants the arguments in a contiguous run, and b and cc need not
        // be one. 14.4 puts the left operand in self^, so it leads: the frame
        // is laid out just past where the answer goes, the way a native call
        // lays one out.
        //
        // 11.3改: and a self^-last one needs nothing else here. Its
        // parameter list is written in operand order too -- the left operand
        // first, the self^ after it -- so the same two slots hold the same two
        // values whichever side the receiver is. Only which slot the body
        // calls self^ differs, and that is the body's own business.
        size_t next_base = rbase + (a) + 1;
        if (next_base + LHAT_MAX_REGISTERS >= LHAT_STACK_SLOTS) {
            return finish(m, chunk, LHAT_RUN_STACK_OVERFLOW, lhat_nil(), at);
        }
        lhat_slots_set(m->slots, next_base + (0), R(b));
        // 11.8改: a unary one declares self^ and nothing else, so the one
        // slot is the whole frame.
        if (!unary) {
            lhat_slots_set(m->slots, next_base + (1), R(cc));
        }
        clear_scratch(m, next_base, carried->proto);

        frame->pc = pc;
        Frame *entered = &m->frames[m->frame_count++];
        entered->closure = carried;
        entered->pc = 0;
        entered->base = next_base;
        entered->result = a;
        entered->prepared = 1;  // 13.8改: an operator answers one value
        entered->cleanup_count = 0;
        entered->returning = false;
        entered->coroutine = NULL;
        entered->disposing = false;
        entered->drop_answer = false;  // 5.3
        // The room is a root while the frame lives (mark_roots), so it
        // starts empty rather than as whatever the slot held before.
        entered->answer = lhat_nil();
        // 11.9: an ordering that reached for '<=>' wants the answer
        // read against zero, not handed over as it is.
        entered->derive = derive_from;
        // 11.9改: unless what answered was an op^=, whose bool^ is the
        // judgement itself -- `derive` then says only whether to negate it.
        entered->derive_equal = answered_bool;

        frame = entered;
        rbase = frame->base;
        chunk = &carried->proto->chunk;
        pc = 0;
        continue;
    }

    drain:
        // Innermost first, one at a time: each body ends with ENDCLEANUP,
        // which comes back here for the next.
        if (frame->cleanup_count > frame->drain_target) {
            pc = frame->cleanups[--frame->cleanup_count];
            continue;
        }
        if (!frame->returning) {
            pc = frame->resume;
            continue;
        }

        // 5.4: whatever still points into this frame takes its value with it,
        // since the slots are about to be reused.
        {
            // 5.3: a tail call from a body that discards what it calls answers
            // for that body, which answers the nil^ of falling off its end.
            LhatValue value = frame->drop_answer ? lhat_nil() : frame->answer;
            // 5.11: the body is over, so the coroutine has nothing left to
            // resume and its cleanups have all run.
            if (frame->coroutine != NULL) {
                frame->coroutine->state = LHAT_COROUTINE_DONE;
                frame->coroutine->cleanup_count = 0;
            }

            // 03 の 4.3: a session's top-level slots outlive the input, so
            // what points into them goes on sharing them. Closing those here
            // would undo the CLOSE compile_session_statements deliberately
            // does not write, and a later input's ':=' through a closure
            // would land in a private copy instead of the slot the next
            // input reads. `kept` is zero everywhere else, which leaves an
            // ordinary frame closed whole the way it always was.
            const LhatProto *ran = frame->closure->proto;
            close_upvalues(m, frame->base + (ran != NULL ? ran->kept : 0));

            // 02 の 10.7: a coroutine dropped in the last few instructions
            // never met a collection, and without one here whether its
            // finally^ runs comes down to whether the heap happened to fill
            // up in time. The frame is still counted, which is what makes
            // this safe -- its registers and its answer are still roots, so
            // this asks the same question every other collection asks and
            // not a wider one. What the program is still holding when the
            // run ends is still held; the run ending is not the machine
            // ending, and lhat_run's own start is what lets the next one go.
            //
            // `ending` says the queue is the only thing keeping the run
            // open. The loop takes one coroutine per turn and comes straight
            // back here, and once nothing is left the frame goes for real.
            // Once only (`end_swept`): a cleanup that drops something has
            // already had its own chance, and the end of a run must not be
            // something a program can go on extending.
            if (m->frame_count == base_depth + 1 && !end_swept) {
                end_swept = true;
                lhat_gc_collect(m);
                if (m->pending_dispose != NULL) {
                    ending = true;
                    pc = at;
                    continue;
                }
            }
            m->frame_count--;

            if (m->frame_count == base_depth) {
                // 05 の 8.9改2: a coroutine's return^ hands its T to the
                // host whole, the same pointer form a yield^'s produce
                // takes. (The checker still refuses one as the program's
                // own answer; a relaxed run that wrote one gets it too --
                // the bytes are real either way.)
                if (lhat_is_hostvalue(value)) {
                    return finish(m, chunk, LHAT_RUN_OK,
                                  hand_hostvalue_out(m, value), at);
                }
                // 02 の 13.8改: several values do cross. The positions rode
                // the frame's own room through the drain; they are copied
                // into the machine's here so the result may point at them
                // once the frame is gone. `value` becomes position 1, which
                // is what lets a host written before tuples read the answer
                // and get something it can use.
                if (lhat_is_run(value)) {
                    size_t positions = lhat_run_width(value);
                    if (positions > LHAT_MAX_TUPLE) {
                        return finish(m, chunk, LHAT_RUN_TUPLE_ARITY,
                                      lhat_nil(), at);
                    }
                    for (size_t i = 0; i < positions; i++) {
                        LhatValue held;
                        held.as = frame->answer_run[i + 1];
                        held.tag = (LhatValueTag)frame->answer_tags[i + 1];
                        m->tuple_scratch[i] = held;
                    }
                    m->tuple_scratch_count = positions;
                    value = positions > 0 ? m->tuple_scratch[0] : lhat_nil();
                }
                return finish(m, chunk, LHAT_RUN_OK, value, at);
            }

            uint8_t into = frame->result;
            // 02 の 13.8改: read off the frame that is going, before `frame`
            // becomes the caller below. The room outlives the pop -- the
            // array entry is still there, only uncounted.
            uint8_t reserved = frame->prepared;
            const LhatValueUnion *answered_run = frame->answer_run;
            const uint8_t *answered_tags = frame->answer_tags;
            // 02 の 11.9: the one frame whose answer is not the value of
            // the expression that made it. An ordering that reached for '<=>'
            // asked a number^ of it, and what was written asks which side of
            // zero that falls on.
            LhatOpcode derived = frame->derive;
            bool derived_equal = frame->derive_equal;
            frame = &m->frames[m->frame_count - 1];
            rbase = frame->base;
            chunk = &frame->closure->proto->chunk;
            pc = frame->pc;
            if (derived != LHAT_FRAME_NO_DERIVE) {
                bool held = false;
                LhatRunStatus status = LHAT_RUN_OK;
                if (derived_equal) {
                    // 11.9改: an op^= answered, and its bool^ is the
                    // judgement. The shape rule asks it for one, so anything
                    // else is a body that did not keep to its signature.
                    if (!lhat_is_bool(value)) {
                        return finish(m, chunk, LHAT_RUN_TYPE_ERROR,
                                      lhat_nil(), at);
                    }
                    held = lhat_as_bool(value) == (derived == LHAT_BC_EQ);
                } else if (derived == LHAT_BC_EQ || derived == LHAT_BC_NE) {
                    held = lhat_value_equal(value, lhat_integer(0)) ==
                           (derived == LHAT_BC_EQ);
                } else if (!ordering(derived, value, lhat_integer(0), &held,
                                     &status)) {
                    // 11.9: the shape rule asks an op^<=> for a number^, so
                    // this is a body that answered with something else.
                    return finish(m, chunk, status, lhat_nil(), at);
                }
                value = lhat_bool(held);
            }
            // 05 の 8.9: a host value answer rides the frame's own room
            // through the drain (see the RETURN case), and is written out
            // whole here, into the caller's slots -- which are live again
            // now that the callee's window is gone.
            // 02 の 13.8改: a tuple rides the same room, and is laid out here
            // as a head slot naming the width plus the positions after it.
            // The two sides have to agree on that width; a disagreement is
            // reported rather than reconciled, because a tuple and a t^{...}
            // are different types and only pack^ turns one into the other.
            // Wherever the checker ran, the type settled this already -- an
            // unchecked compile, 03 の 4.3's session and 05 の 5.3's
            // separately compiled units are what land here.
            if (lhat_is_run(value)) {
                size_t positions = lhat_run_width(value);
                if (reserved <= 1) {
                    return finish(m, chunk, LHAT_RUN_TUPLE_UNEXPECTED,
                                  lhat_nil(), at);
                }
                // 13.8改: a narrower run is the short side of a widened
                // fold and lands as it is -- CHECKRUN pads the missing
                // positions with nil^ where the binding takes it apart.
                // Only a wider run than the reservation is the mismatch.
                if ((size_t)reserved < positions + 1 ||
                    rbase + into + positions >= LHAT_STACK_SLOTS) {
                    return finish(m, chunk, LHAT_RUN_TUPLE_ARITY, lhat_nil(),
                                  at);
                }
                lhat_slots_set(m->slots, rbase + into, value);
                for (size_t i = 0; i < positions; i++) {
                    LhatValue held;
                    held.as = answered_run[i + 1];
                    held.tag = (LhatValueTag)answered_tags[i + 1];
                    lhat_slots_set(m->slots, rbase + into + 1 + i, held);
                }
            } else if (lhat_is_hostvalue(value)) {
                // 05 の 8.9: before the run-reservation reading -- a call
                // that answers a host value declared its width as prepared,
                // and the whole value is what belongs at the slot. A site
                // that could not know the type reserved one slot, and is
                // refused rather than overwritten past.
                if ((size_t)reserved < value.as.hostvalue_run[0].hostvalue->width ||
                    !place_hostvalue_answer(m, rbase + into, value)) {
                    return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(),
                                  at);
                }
            } else if (reserved > 1) {
                // 13.8改 with 04 の 3.1: the call site reserved a run and one
                // value came back. Not a fault by itself -- '(A, B)|SomeError'
                // answers its error arm as one value, and the head slot is
                // where it belongs so that ISERROR reads it. What tells that
                // apart from a callee that simply answered wrong is CHECKRUN,
                // emitted on the path past the error.
                SET_R(into, value);
            } else {
                SET_R(into, value);
            }
        }
    }

    return finish(m, chunk, LHAT_RUN_OK, lhat_nil(), chunk->count);
}

LhatRunResult lhat_run_arguments(LhatMachine *m, const LhatProto *proto,
                                 const LhatValue *arguments, size_t count)
{
    const LhatChunk *chunk = &proto->chunk;

    // 02 の 13.8改: whatever the last answer left in the room is not this
    // run.s, and holding it would keep those values from being collected.
    // 8.7改2: a panic asked for outside any host call belongs to no run.
    m->host_panicked = false;
    m->tuple_scratch_count = 0;

    // 5.4 and 5.2: the frames and the registers belong to the run, so each
    // starts with neither. What the heap holds is the machine's and stays.
    //
    // 03 の 4.3: except the registers the unit says are already spoken for.
    // A REPL's second input finds the top-level names of the first there,
    // and clearing them would be clearing the session.
    // 5.4: what an earlier run left sharing a slot this one is about to
    // clear takes its value away first. The slots below `reserved` are the
    // session's and survive, so what points into them stays open and shared
    // -- that is what lets a closure an earlier input made go on naming the
    // very place a later one reads and writes.
    m->frame_count = 0;
    // 04 の 11.6改: and what the last run's fault left readable goes with
    // them -- a host asked "where am I" in this run must be answered with
    // this run's frames, not the span the last one recorded.
    m->fault_depth = m->fault_base = 0;
    close_upvalues(m, proto->reserved);
    for (size_t i = proto->reserved; i < LHAT_STACK_SLOTS; i++) {
        lhat_slots_set(m->slots, i, lhat_nil());
    }

    LhatClosure *entry =
        (LhatClosure *)lhat_object_alloc(&m->objects, sizeof *entry,
                                        LHAT_OBJECT_SUBROUTINE);
    if (entry == NULL) {
        return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), 0);
    }
    entry->proto = proto;

    Frame *frame = &m->frames[m->frame_count++];
    frame->closure = entry;
    frame->pc = 0;
    frame->base = 0;
    frame->result = 0;
    frame->prepared = 1;  // 13.8改: a unit's answer leaves through one slot
    frame->cleanup_count = 0;
    frame->returning = false;
    frame->coroutine = NULL;
    frame->disposing = false;
    frame->derive = LHAT_FRAME_NO_DERIVE;
    frame->derive_equal = false;
    frame->drop_answer = false;  // 5.3
    frame->answer = lhat_nil();

    // 02 の 13.7 with 05 の 3.2: a script's '...' is register 0, and what was
    // handed over is collected into it the way a CALL collects. A module^
    // unit (or a session's input) has no '...' to take anything.
    if (proto->has_variadic) {
        LhatTable *collected = lhat_table_new(&m->objects);
        if (collected == NULL) {
            return finish(m, chunk, LHAT_RUN_OUT_OF_MEMORY, lhat_nil(), 0);
        }
        for (size_t i = 0; i < count; i++) {
            bool refused = false;
            if (lhat_is_hostvalue(arguments[i]) ||
                !set_key(m, collected, lhat_integer((int64_t)i + 1),
                         arguments[i], &refused)) {
                return finish(m, chunk, LHAT_RUN_TYPE_ERROR, lhat_nil(), 0);
            }
        }
        lhat_slots_set(m->slots, proto->reserved,
                       lhat_object((LhatObject *)collected));
    } else if (count > 0) {
        return finish(m, chunk, LHAT_RUN_ARITY, lhat_nil(), 0);
    }

    return run_frames(m, 0, false);
}

LhatRunResult lhat_run(LhatMachine *m, const LhatProto *proto)
{
    return lhat_run_arguments(m, proto, NULL, 0);
}

// A LhatRunResult for a call that never got as far as pushing a frame -- no
// chunk exists yet to name a line or an operator from, so both come back
// empty the way finish() already leaves them for `at` out of range.
static LhatRunResult call_fault(Machine *m, LhatRunStatus status)
{
    // 04 の 11.6改: a boundary fault happened before any frame went on, so
    // the recorded span is empty -- lhat_machine_fault_depth answers 0.
    if (status != LHAT_RUN_OK) {
        m->fault_base = m->fault_depth = m->frame_count;
        m->fault_at = 0;
    }
    LhatRunResult result;
    result.status = status;
    result.value = lhat_nil();
    result.positions = NULL;
    result.position_count = 0;
    result.at = 0;
    result.line = 0;
    result.op_name = NULL;
    result.op_name_length = 0;
    result.collected = m->collected;
    result.live = m->objects.count;
    return result;
}

// The body of both host entry points below. `receiver` means something only
// when `as_method`, and then the callee is a member reached through it
// (14.4) -- which is the whole of the difference between them.
static LhatRunResult host_call(Machine *m, LhatValue callee, LhatValue receiver,
                               bool as_method, const LhatValue *arguments,
                               size_t count)
{
    size_t base = m->frame_count;
    m->tuple_scratch_count = 0;  // 13.8改, as in lhat_run

    // 05 の 8.7: a member the host registered in C, called back the way an
    // instruction calls one -- flat arguments, no spread. Without this a
    // host holding a hostdata value could call nothing on it, since 8.8
    // registers every member of one this way.
    if (lhat_is_object_kind(callee, LHAT_OBJECT_HOST)) {
        const LhatHost *host = (const LhatHost *)lhat_as_object(callee);
        // 13.4 keeps self^ out of the count, as at a compiled call site.
        if (host->has_variadic ? count < host->parameters
                               : count != host->parameters) {
            return call_fault(m, LHAT_RUN_ARITY);
        }
        bool receiver_first = host->takes_self && as_method;
        size_t given = count + (receiver_first ? 1 : 0);
        LhatValue gathered[LHAT_MAX_REGISTERS + 2];
        if (given > LHAT_MAX_REGISTERS + 2) {
            return call_fault(m, LHAT_RUN_ARITY);
        }
        size_t into = 0;
        if (receiver_first) {
            gathered[into++] = receiver;
        }
        for (size_t i = 0; i < count; i++) {
            gathered[into++] = arguments[i];
        }
        // 05 の 8.8: a dispose^ called by hand is the same giving-back the
        // collection would do, so it is marked here and 10.7 keeps the
        // sweep from doing it again -- as at a compiled call site.
        if (receiver_first &&
            lhat_is_object_kind(gathered[0], LHAT_OBJECT_HOSTDATA)) {
            LhatHostData *data = (LhatHostData *)lhat_as_object(gathered[0]);
            const LhatHostDataTag *by = lhat_hostdata_releaser(data->tag);
            if (by != NULL && by->release == host->call) {
                data->released = true;
            }
        }
        size_t frames_before = m->frame_count;
        LhatValue answered = lhat_nil();
        if (!call_host_fn(m, host->call, host->context, gathered, given,
                          &answered)) {
            return call_fault(m, LHAT_RUN_TUPLE_ARITY);
        }
        // 05 の 8.7改2: the host's own fault comes back to the host that
        // called it, as it would to an instruction. The span is empty
        // (call_fault) -- no frame of this machine's was involved.
        LhatRunStatus left = LHAT_RUN_OK;
        LhatValue left_with = lhat_nil();
        if (host_faulted(m, frames_before, &left, &left_with)) {
            LhatRunResult faulted = call_fault(m, left);
            faulted.value = left_with;
            return faulted;
        }
        // 05 の 8.9: a host value cannot cross this boundary -- its bytes
        // live in slots the boundary does not have -- so it goes the way a
        // return's base already sends one.
        if (lhat_is_hostvalue(answered)) {
            answered = lhat_nil();
        }
        LhatRunResult made = call_fault(m, LHAT_RUN_OK);
        if (lhat_is_run(answered)) {
            // 02 の 13.8改: the positions already sit in the machine's room
            // (the boundary's own room), which is where the result aims anyway.
            size_t positions = lhat_run_width(answered);
            if (positions != m->tuple_scratch_count) {
                return call_fault(m, LHAT_RUN_TUPLE_ARITY);
            }
            made.positions = m->tuple_scratch;
            made.position_count = positions;
            made.value = positions > 0 ? m->tuple_scratch[0] : lhat_nil();
        } else {
            m->tuple_scratch_count = 0;
            made.value = answered;
        }
        return made;
    }

    // 05 の 8.7's LhatHostFn has no shape for a spread; a host calling back in
    // already has its arguments as a flat array, so this is the plain-call
    // subset of what LHAT_BC_CALL does (vm.c's CALL case).
    if (!lhat_is_object_kind(callee, LHAT_OBJECT_SUBROUTINE)) {
        return call_fault(m, LHAT_RUN_NOT_CALLABLE);
    }
    const LhatClosure *closure = (const LhatClosure *)lhat_as_object(callee);
    if (closure->proto == NULL) {
        return call_fault(m, LHAT_RUN_NOT_CALLABLE);
    }

    // 14.4: a member that takes self^ is handed the receiver in a slot of its
    // own, and one that does not is a static member -- the receiver is then
    // simply not passed, the same way LHAT_BC_CALLMETHOD steps over it.
    bool pass_self = as_method && closure->proto->takes_self;

    size_t declared_slots = closure->proto->parameters;
    size_t required =
        closure->proto->has_variadic ? declared_slots - 1 : declared_slots;
    // 13.4 keeps self^ out of the parameter list a call writes, and
    // proto->parameters counts the slot it occupies -- so what the host
    // handed over is compared after the receiver is added back in.
    size_t written = count + (pass_self ? 1 : 0);
    if (closure->proto->has_variadic ? written < required
                                     : written != declared_slots) {
        return call_fault(m, LHAT_RUN_ARITY);
    }

    // 02 の 11.3改: an op^ may write self^ last instead, which says the right
    // operand is the receiver. The slot it occupies moves with it, so where
    // the receiver lands is read off the proto rather than assumed.
    size_t self_at =
        pass_self && closure->proto->self_last && required > 0 ? required - 1
                                                               : 0;

    // 02 の 15.5: calling a yieldable procedure does not suspend the caller.
    // It answers a coroutine, the body not started -- exactly what a
    // compiled CALL answers, handed across this boundary instead. The
    // arguments go into the coroutine's saved registers, where the first
    // resume (lhat_machine_resume) finds them laid out as a frame's. No
    // frame is pushed, so the stack is not asked for room.
    if (closure->proto->yields) {
        LhatCoroutine *co = lhat_coroutine_new(
            &m->objects, closure, closure->proto->chunk.registers);
        if (co == NULL) {
            return call_fault(m, LHAT_RUN_OUT_OF_MEMORY);
        }
        size_t taken = 0;
        for (size_t i = 0; i < required; i++) {
            if (pass_self && i == self_at) {
                lhat_slots_set(co->registers, i, receiver);
                continue;
            }
            lhat_slots_set(co->registers, i, arguments[taken++]);
        }
        if (closure->proto->has_variadic) {
            LhatTable *collected = lhat_table_new(&m->objects);
            if (collected == NULL) {
                return call_fault(m, LHAT_RUN_OUT_OF_MEMORY);
            }
            for (size_t i = taken; i < count; i++) {
                bool refused = false;
                // As at the compiled call: the collection is a table.
                if (lhat_is_hostvalue(arguments[i])) {
                    return call_fault(m, LHAT_RUN_TYPE_ERROR);
                }
                if (!set_key(m, collected,
                             lhat_integer((int64_t)(i - taken + 1)),
                             arguments[i], &refused)) {
                    return call_fault(m, LHAT_RUN_OUT_OF_MEMORY);
                }
            }
            lhat_slots_set(co->registers, required,
                           lhat_object((LhatObject *)collected));
        }
        // call_fault fills the empty result; only the value differs.
        LhatRunResult made = call_fault(m, LHAT_RUN_OK);
        made.value = lhat_object((LhatObject *)co);
        return made;
    }

    if (base >= LHAT_MAX_FRAMES) {
        return call_fault(m, LHAT_RUN_STACK_OVERFLOW);
    }

    // Mirrors 6249's "just past what the caller's frame uses" -- a native
    // caller has no R(a) of its own to measure from, so the frame
    // already on top (if there is one) stands in for it. base == 0 is
    // lhat_run's own case: nothing is on the stack yet, so frame 0 starts at
    // its beginning.
    size_t next_base =
        base == 0 ? 0
                  : m->frames[base - 1].base +
                        m->frames[base - 1].closure->proto->chunk.registers;
    if (next_base + LHAT_MAX_REGISTERS >= LHAT_STACK_SLOTS) {
        return call_fault(m, LHAT_RUN_STACK_OVERFLOW);
    }

    size_t next = 0;
    for (size_t i = 0; i < required; i++) {
        if (pass_self && i == self_at) {
            lhat_slots_set(m->slots, next_base + (i), receiver);
            continue;
        }
        lhat_slots_set(m->slots, next_base + (i), arguments[next++]);
    }
    if (closure->proto->has_variadic) {
        LhatTable *collected = lhat_table_new(&m->objects);
        if (collected == NULL) {
            return call_fault(m, LHAT_RUN_OUT_OF_MEMORY);
        }
        for (size_t i = next; i < count; i++) {
            bool refused = false;
            if (!set_key(m, collected, lhat_integer((int64_t)(i - next + 1)),
                         arguments[i], &refused)) {
                return call_fault(m, LHAT_RUN_OUT_OF_MEMORY);
            }
        }
        lhat_slots_set(m->slots, next_base + (required), lhat_object((LhatObject *)collected));
    }
    clear_scratch(m, next_base, closure->proto);

    Frame *called = &m->frames[m->frame_count++];
    called->closure = closure;
    called->pc = 0;
    called->base = next_base;
    called->result = 0;  // never read: base_depth's drain returns instead
    // 13.8改: the host boundary takes one value (LhatRunResult.value), so a
    // tuple never crosses it. pack^ is what a body answering one uses here.
    called->prepared = 1;
    called->cleanup_count = 0;
    called->returning = false;
    called->coroutine = NULL;
    called->disposing = false;
    called->derive = LHAT_FRAME_NO_DERIVE;
    called->derive_equal = false;
    called->drop_answer = false;  // 5.3
    called->answer = lhat_nil();

    return run_frames(m, base, false);
}

LhatRunResult lhat_machine_call(LhatMachine *machine, LhatValue callee,
                                const LhatValue *arguments, size_t count)
{
    return host_call((Machine *)machine, callee, lhat_nil(), false, arguments,
                     count);
}

LhatRunResult lhat_machine_run_seeded(Machine *m, const LhatClosure *closure,
                                      const LhatValue *seed, size_t count)
{
    size_t base = m->frame_count;
    m->tuple_scratch_count = 0;  // 13.8改, as in lhat_run
    if (closure == NULL || closure->proto == NULL ||
        count > closure->proto->chunk.registers) {
        return call_fault(m, LHAT_RUN_NOT_CALLABLE);
    }
    if (base >= LHAT_MAX_FRAMES) {
        return call_fault(m, LHAT_RUN_STACK_OVERFLOW);
    }
    size_t next_base =
        base == 0 ? 0
                  : m->frames[base - 1].base +
                        m->frames[base - 1].closure->proto->chunk.registers;
    if (next_base + LHAT_MAX_REGISTERS >= LHAT_STACK_SLOTS) {
        return call_fault(m, LHAT_RUN_STACK_OVERFLOW);
    }
    for (size_t i = 0; i < count; i++) {
        lhat_slots_set(m->slots, next_base + i, seed[i]);
    }
    // The rest of the window is emptied: the collector walks a frame's whole
    // width, and whatever an earlier run left there is not this one's.
    for (size_t i = count; i < closure->proto->chunk.registers; i++) {
        lhat_slots_set(m->slots, next_base + i, lhat_nil());
    }

    Frame *called = &m->frames[m->frame_count++];
    called->closure = closure;
    called->pc = 0;
    called->base = next_base;
    called->result = 0;  // never read: base_depth's drain returns instead
    called->prepared = 1;
    called->cleanup_count = 0;
    called->returning = false;
    called->coroutine = NULL;
    called->disposing = false;
    called->derive = LHAT_FRAME_NO_DERIVE;
    called->derive_equal = false;
    called->drop_answer = false;
    called->answer = lhat_nil();

    LhatRunResult ran = run_frames(m, base, false);
    if (ran.status != LHAT_RUN_OK) {
        // As run_one_disposal abandons a failed cleanup: closed first, or a
        // place captured inside the evaluation would be left pointing at
        // slots the next call reuses.
        close_upvalues(m, next_base);
        m->frame_count = base;
        m->fault_base = 0;
        m->fault_depth = 0;
    }
    return ran;
}

static size_t waiting_disposals(const Machine *m)
{
    size_t waiting = 0;
    for (const LhatCoroutine *co = m->pending_dispose; co != NULL;
         co = co->next_pending) {
        waiting++;
    }
    return waiting;
}

// 02 の 10.7: runs the cleanups of one dropped coroutine, from C. The frame
// goes just past whatever is on top -- the same measure host_call takes, and
// base == 0 (nothing running) is as good a case as any -- and the loop is
// entered at the drain, since a disposal frame has no instructions of its
// own to run.
//
// False when there was nothing waiting or nowhere to put the frame. What the
// cleanups answered comes back in `status`.
static bool run_one_disposal(Machine *m, LhatRunStatus *status)
{
    size_t base = m->frame_count;
    if (m->pending_dispose == NULL || base >= LHAT_MAX_FRAMES) {
        return false;
    }
    size_t next_base =
        base == 0 ? 0
                  : m->frames[base - 1].base +
                        m->frames[base - 1].closure->proto->chunk.registers;
    if (next_base + LHAT_MAX_REGISTERS >= LHAT_STACK_SLOTS) {
        return false;
    }

    // gc.c only ever holds back a suspended BODY coroutine with cleanups
    // left, so the closure and the proto this reads through are there.
    LhatCoroutine *co = lhat_gc_take_pending(m);
    Frame *frame = NULL;
    size_t rbase = 0;
    const LhatChunk *chunk = NULL;
    size_t pc = 0;
    enter_disposal_frame(m, co, next_base, 0, &frame, &rbase, &chunk, &pc);
    // 15.4: same as an explicit dispose -- the slot a yield^ answers into
    // gets nil^, since nothing sent anything in.
    lhat_slots_set(m->slots, rbase + co->sent_into, lhat_nil());
    *status = run_frames(m, base, true).status;

    // 04 の 11.6改: a fault leaves its frames standing so that a traceback
    // can be read off them. There is no caller here to read one -- the
    // machine was tidying up after itself, and the run that dropped this
    // coroutine is long over -- and leaving them would be worse than
    // useless. The frames hold the closure this coroutine was suspended in,
    // and so a body a host may be about to free, while
    // lhat_machine_pending_disposals -- which is what it asks before freeing
    // -- would read zero. So the cleanup is abandoned and its frames go.
    //
    // 5.4: closed first, or a place captured inside the cleanup would be
    // left pointing at slots the next call reuses.
    if (*status != LHAT_RUN_OK) {
        close_upvalues(m, next_base);
        m->frame_count = base;
        m->fault_base = 0;
        m->fault_depth = 0;
    }
    return true;
}

size_t lhat_machine_collectgarbage(LhatMachine *machine)
{
    Machine *m = (Machine *)machine;
    if (m == NULL) {
        return 0;
    }
    lhat_gc_collect(m);

    // 02 の 10.7: and here the cleanups the collection held back get to run,
    // which inside a run is what an instruction boundary does for them. A
    // host calling this has no loop under it to do that, and the difference
    // matters: until they have run, a dropped coroutine still holds the
    // closure it was suspended in.
    //
    // Never on top of a disposal already under way -- two unwindings that
    // have nothing to do with each other must not interleave, which is the
    // same refusal the run loop makes.
    if (m->frame_count > 0 && m->frames[m->frame_count - 1].disposing) {
        return m->objects.count;
    }

    // Only what is queued now. A cleanup that drops another coroutine leaves
    // it for the next call -- the bound the run loop's `end_swept` puts on
    // the end of a run, for the same reason: this must not be something a
    // program can go on extending.
    size_t waiting = waiting_disposals(m);
    bool ran_any = false;
    for (size_t i = 0; i < waiting; i++) {
        LhatRunStatus status = LHAT_RUN_OK;
        if (!run_one_disposal(m, &status)) {
            break;
        }
        ran_any = true;
        // A cleanup that faulted stops the drain: the rest keep, and
        // lhat_machine_fault_* says what happened to this one.
        if (status != LHAT_RUN_OK) {
            break;
        }
    }

    // Their bodies have finished now, so they are DONE with no cleanups left
    // and gc.c will not hold them back again -- this is the cycle that
    // actually reclaims them.
    if (ran_any) {
        lhat_gc_collect(m);
    }
    return m->objects.count;
}

// 02 の 14.16: typeof^ from C. One reading for both sides, so a host and a
// program never learn different things about one value -- which is why
// this is tag_type itself and not a second walk beside it.
const LhatRuntimeType *lhat_value_type(LhatMachine *machine,
                                       LhatValue value)
{
    Machine *m = (Machine *)machine;
    if (m == NULL) {
        return NULL;
    }
    return tag_type(&m->objects, value);
}

size_t lhat_machine_pending_disposals(const LhatMachine *machine)
{
    return machine != NULL ? waiting_disposals((const Machine *)machine) : 0;
}

bool lhat_machine_holds_body(const LhatMachine *machine,
                             const LhatProto *const *bodies, size_t count)
{
    if (machine == NULL || bodies == NULL || count == 0) {
        return false;
    }
    // The heap's chain holds every object the machine has, live or not yet
    // swept -- which is why the collector goes first (vm.h). Closures name
    // their body outright; a coroutine names it through the closure it was
    // suspended in (a host walk has none). Everything else holds no body.
    for (const LhatObject *object = machine->objects.objects; object != NULL;
         object = object->next) {
        const LhatProto *proto = NULL;
        if (object->kind == LHAT_OBJECT_SUBROUTINE) {
            proto = ((const LhatClosure *)object)->proto;
        } else if (object->kind == LHAT_OBJECT_COROUTINE) {
            const LhatCoroutine *coroutine = (const LhatCoroutine *)object;
            proto =
                coroutine->closure != NULL ? coroutine->closure->proto : NULL;
        }
        if (proto == NULL) {
            continue;
        }
        for (size_t i = 0; i < count; i++) {
            if (bodies[i] == proto) {
                return true;
            }
        }
    }
    return false;
}

LhatRunResult lhat_machine_call_member(LhatMachine *machine,
                                       LhatValue receiver, const char *name,
                                       size_t length,
                                       const LhatValue *arguments,
                                       size_t count)
{
    Machine *m = (Machine *)machine;
    if (name == NULL || count > LHAT_MAX_REGISTERS) {
        return call_fault(m, LHAT_RUN_TYPE_ERROR);
    }
    // 14.4 reaches a member through a table; a value that is not one has no
    // members to reach, which is the same refusal an instruction makes.
    // 05 の 8.8: a hostdata value reads its members through the registered
    // type's table, exactly as an instruction's read does -- which is why
    // this is readable_table rather than table_of.
    const LhatTable *table = readable_table(receiver);
    if (table == NULL) {
        return call_fault(m, LHAT_RUN_TYPE_ERROR);
    }

    LhatString *key = lhat_string_new(&m->objects, name, length);
    if (key == NULL) {
        return call_fault(m, LHAT_RUN_OUT_OF_MEMORY);
    }
    // 14.7: an instance sees its definition's members too, and lhat_table_get
    // is what already walks that.
    LhatValue member = lhat_table_get(table, lhat_object((LhatObject *)key));

    // 14.12: at most one candidate fits, so this is a search and not a
    // choice. The lineup is what an instruction has lying in its registers:
    // the callee, the receiver, then what the call wrote.
    if (lhat_is_object_kind(member, LHAT_OBJECT_OVERLOAD)) {
        const LhatOverload *group = (const LhatOverload *)lhat_as_object(member);
        LhatValue lineup[LHAT_MAX_REGISTERS + 2];
        lineup[0] = member;
        lineup[1] = receiver;
        for (size_t i = 0; i < count; i++) {
            lineup[i + 2] = arguments[i];
        }
        size_t picked_skip = 1;
        LhatValue chosen = lhat_nil();
        for (size_t i = 0; i < group->count; i++) {
            if (fits_call(group->candidates[i], lineup, (uint8_t)count, true,
                          &picked_skip)) {
                chosen = group->candidates[i];
                break;
            }
        }
        if (lhat_is_nil(chosen)) {
            return call_fault(m, LHAT_RUN_NO_CANDIDATE);
        }
        member = chosen;
    }

    return host_call(m, member, receiver, true, arguments, count);
}

bool lhat_machine_make_coroutine(LhatMachine *machine, LhatHostStepFn step,
                                 void *context, LhatHostFn release,
                                 LhatValue held, LhatValue *out)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || step == NULL || out == NULL) {
        return false;
    }
    LhatCoroutine *walk =
        lhat_host_iterator(&m->objects, step, context, release, held);
    if (walk == NULL) {
        return false;
    }
    *out = lhat_object((LhatObject *)walk);
    return true;
}

LhatRunResult lhat_machine_resume(LhatMachine *machine, LhatValue coroutine,
                                  const LhatValue *sent, size_t sent_count)
{
    Machine *m = (Machine *)machine;
    m->tuple_scratch_count = 0;  // 13.8改, as in host_call
    if (sent == NULL) {
        sent_count = 0;
    }
    if (!lhat_is_object_kind(coroutine, LHAT_OBJECT_COROUTINE) ||
        sent_count > LHAT_MAX_TUPLE) {
        return call_fault(m, LHAT_RUN_TYPE_ERROR);
    }
    LhatCoroutine *co = (LhatCoroutine *)lhat_as_object(coroutine);
    // 15.2's guards as the instructions make them -- resume subsumes start
    // here (lua_resume's shape): a fresh body runs from the top, and that
    // first `sent` is discarded, since no yield^ awaits it yet.
    if (co->state == LHAT_COROUTINE_DONE ||
        co->state == LHAT_COROUTINE_RUNNING) {
        return call_fault(m, LHAT_RUN_DEAD_COROUTINE);
    }

    // 16.3: a table's walk has no body, so one step is the whole of the
    // resume -- no frame entered. The pair crosses the boundary as the
    // positions of a tuple (13.8改), the way any tuple crosses it.
    if (co->source == LHAT_COROUTINE_TABLE) {
        LhatValue key, value;
        if (!lhat_table_walk(co, &key, &value)) {
            co->state = LHAT_COROUTINE_DONE;
            return call_fault(m, LHAT_RUN_OK);
        }
        co->state = LHAT_COROUTINE_SUSPENDED;
        LhatRunResult made = call_fault(m, LHAT_RUN_OK);
        if (co->part != LHAT_WALK_PAIR) {
            made.value = co->part == LHAT_WALK_KEYS ? key : value;
            return made;
        }
        m->tuple_scratch[0] = key;
        m->tuple_scratch[1] = value;
        m->tuple_scratch_count = 2;
        made.value = key;
        made.positions = m->tuple_scratch;
        made.position_count = 2;
        return made;
    }

    // 05 の 8.8: and a host's walk is one step of its own C body. RUNNING
    // above already refuses a step resuming its own coroutine. 13.8改:
    // everything the resume sent goes over, the way it does to a body.
    if (co->source == LHAT_COROUTINE_HOST) {
        LhatValue out = lhat_nil();
        int written = 0;
        co->state = LHAT_COROUTINE_RUNNING;
        m->tuple_scratch_count = 0;
        bool more = co->step(machine, co->host_state, sent, sent_count,
                             m->tuple_scratch, &written);
        if (!boundary_answer(m, written, &out)) {
            return call_fault(m, LHAT_RUN_TUPLE_ARITY);
        }
        if (!more) {
            co->state = LHAT_COROUTINE_DONE;
            lhat_coroutine_release((LhatObject *)co, machine);
            // 13.9: what it wrote as it ended is T, and a resume from C
            // reads it the way a written one does. Nothing written is a
            // walk that ended with nothing.
        } else {
            co->state = LHAT_COROUTINE_SUSPENDED;
            if (written == 0) {
                return call_fault(m, LHAT_RUN_TUPLE_ARITY);
            }
        }
        // 05 の 8.9改2: a host value passes through whole -- the step wrote
        // it into the machine's scratch itself (lhat_make_hostvalue), which
        // is exactly where the caller reads the pointer form.
        LhatRunResult made = call_fault(m, LHAT_RUN_OK);
        if (lhat_is_run(out)) {
            size_t positions = lhat_run_width(out);
            if (positions != m->tuple_scratch_count) {
                return call_fault(m, LHAT_RUN_TUPLE_ARITY);
            }
            made.positions = m->tuple_scratch;
            made.position_count = positions;
            made.value = positions > 0 ? m->tuple_scratch[0] : lhat_nil();
        } else {
            made.value = out;
        }
        return made;
    }

    // A body: the frame goes back on at the base of a run of its own --
    // host_call's placement, re-entered where the yield^ left off, or at
    // the top when the body has not started. The yield that suspends it
    // again leaves through LHAT_BC_YIELD's base case, the way a return
    // leaves through its own.
    if (co->closure == NULL || co->closure->proto == NULL) {
        return call_fault(m, LHAT_RUN_NOT_CALLABLE);
    }
    // 13.8改: the count a resume sends is the R's -- checked here the way
    // the natives check it, with the same tolerance for a proto the checker
    // never reached, and the same stop for a run that would not fit the
    // suspended frame. A fresh body's first send is discarded, so any count
    // passes there.
    // 05 の 8.9改2: a host value rides the send whole -- the pointer form
    // the host already holds (lhat_make_hostvalue's scratch, or an argument
    // echoed back), one seat, never mixed into a run. The lay-down side
    // expands it (enter_resume_frame), and unlike a compiled resume the
    // pointer never aims into the registers about to be restored over.
    for (size_t i = 0; i < sent_count; i++) {
        if (lhat_is_hostvalue(sent[i]) && sent_count > 1) {
            return call_fault(m, LHAT_RUN_TYPE_ERROR);
        }
    }
    if (co->state == LHAT_COROUTINE_SUSPENDED) {
        const LhatProto *from = co->closure->proto;
        // As at the natives' resume: an unknown proto keeps the old ceiling
        // of one, since only a checked body has reserved the run's slots.
        if (from->yield_receives_known
                ? sent_count != from->yield_receive_count
                : sent_count > 1) {
            return call_fault(m, LHAT_RUN_ARITY);
        }
        if (sent_count > 1 &&
            (size_t)co->sent_into + sent_count >= co->register_count) {
            return call_fault(m, LHAT_RUN_ARITY);
        }
        if (sent_count == 1 && lhat_is_hostvalue(sent[0])) {
            const LhatValueUnion *srun = sent[0].as.hostvalue_run;
            const LhatHostValueTag *stag =
                srun != NULL ? srun[0].hostvalue : NULL;
            // The lowered R is what the body reserved its slots by, so a
            // checked body holds the send to its own tag -- stronger than
            // the count check alone. An unchecked one still gets the room
            // checked.
            const struct LhatRuntimeType *wants = from->yield_receive_type;
            if (stag == NULL ||
                (wants != NULL && (wants->kind != LHAT_TYPE_RT_HOSTVALUE ||
                                   wants->hostvalue_tag != stag)) ||
                (size_t)co->sent_into + stag->width > co->register_count) {
                return call_fault(m, LHAT_RUN_TYPE_ERROR);
            }
        }
    }
    size_t base = m->frame_count;
    if (base >= LHAT_MAX_FRAMES) {
        return call_fault(m, LHAT_RUN_STACK_OVERFLOW);
    }
    size_t next_base =
        base == 0 ? 0
                  : m->frames[base - 1].base +
                        m->frames[base - 1].closure->proto->chunk.registers;
    if (next_base + LHAT_MAX_REGISTERS >= LHAT_STACK_SLOTS) {
        return call_fault(m, LHAT_RUN_STACK_OVERFLOW);
    }
    // prepared = 1: the host boundary takes one value, as host_call says.
    // The result slot is never read -- the base cases return instead.
    enter_resume_frame(m, co, next_base, 0, 1, sent, sent_count);
    return run_frames(m, base, false);
}

bool lhat_machine_coroutine_done(LhatValue coroutine)
{
    return lhat_is_object_kind(coroutine, LHAT_OBJECT_COROUTINE) &&
           ((const LhatCoroutine *)lhat_as_object(coroutine))->state ==
               LHAT_COROUTINE_DONE;
}

bool lhat_machine_make_string(LhatMachine *machine, const char *text,
                              size_t length, LhatValue *out)
{
    Machine *m = (Machine *)machine;
    LhatString *string = lhat_string_new(&m->objects, text, length);
    if (string == NULL) {
        return false;
    }
    *out = lhat_object((LhatObject *)string);
    return true;
}

bool lhat_machine_make_closure(LhatMachine *machine, const LhatProto *proto,
                               LhatValue *out)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || proto == NULL || proto->upvalue_count != 0) {
        return false;
    }
    LhatClosure *closure = (LhatClosure *)lhat_object_alloc(
        &m->objects, sizeof *closure, LHAT_OBJECT_SUBROUTINE);
    if (closure == NULL) {
        return false;
    }
    closure->proto = proto;
    // calloc 起源(lhat_object_alloc)なので upvalues/upvalue_count は
    // 既に NULL/0 -- proto->upvalue_count == 0 の前提と一致する。
    *out = lhat_object((LhatObject *)closure);
    return true;
}

static void own_tree(LhatProto *proto, LhatObject *owner)
{
    proto->owner = owner;
    for (size_t i = 0; i < proto->proto_count; i++) {
        own_tree(proto->protos[i], owner);
    }
}

bool lhat_machine_adopt_script(LhatMachine *machine, LhatProto *proto,
                               LhatValue *out)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || proto == NULL || proto->upvalue_count != 0) {
        return false;
    }
    LhatLoadedScript *script = (LhatLoadedScript *)lhat_object_alloc(
        &m->objects, sizeof *script, LHAT_OBJECT_SCRIPT);
    if (script == NULL) {
        return false;
    }
    script->root = proto;
    own_tree(proto, (LhatObject *)script);
    // The collector runs between instructions and no further, so the script
    // is still there when the closure that reaches it is made.
    return lhat_machine_make_closure(machine, proto, out);
}

void lhat_machine_panic(LhatMachine *machine, LhatValue value)
{
    Machine *m = (Machine *)machine;
    if (m == NULL) {
        return;
    }
    // Rooted the way a fault's value is (gc.c), and read back by
    // host_faulted when the host function returns.
    m->fault_value = value;
    m->host_panicked = true;
}

bool lhat_machine_panic_text(LhatMachine *machine, const char *text)
{
    LhatValue message = lhat_nil();
    if (!lhat_machine_make_string(machine, text, strlen(text), &message)) {
        return false;
    }
    lhat_machine_panic(machine, message);
    return true;
}

bool lhat_machine_make_cell(LhatMachine *machine, LhatValue held,
                            LhatUpvalue **out)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || out == NULL) {
        return false;
    }
    LhatUpvalue *cell = (LhatUpvalue *)lhat_object_alloc(
        &m->objects, sizeof *cell, LHAT_OBJECT_UPVALUE);
    if (cell == NULL) {
        return false;
    }
    lhat_ref_set(lhat_upvalue_closed_ref(cell), held);
    cell->location = lhat_upvalue_closed_ref(cell);
    cell->next_open = NULL;
    *out = cell;
    return true;
}

bool lhat_machine_make_closure_with(LhatMachine *machine,
                                    const LhatProto *proto,
                                    LhatUpvalue *const *cells, size_t count,
                                    LhatValue *out)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || proto == NULL || out == NULL ||
        proto->upvalue_count != count || (count > 0 && cells == NULL)) {
        return false;
    }
    LhatClosure *closure = (LhatClosure *)lhat_object_alloc(
        &m->objects, sizeof *closure, LHAT_OBJECT_SUBROUTINE);
    if (closure == NULL) {
        return false;
    }
    closure->proto = proto;
    closure->upvalue_count = count;
    if (count > 0) {
        closure->upvalues =
            (LhatUpvalue **)lhat_calloc(count, sizeof *closure->upvalues);
        if (closure->upvalues == NULL) {
            return false;
        }
        for (size_t i = 0; i < count; i++) {
            if (cells[i] == NULL) {
                return false;
            }
            closure->upvalues[i] = cells[i];
        }
    }
    *out = lhat_object((LhatObject *)closure);
    return true;
}

const LhatProto *lhat_closure_proto(LhatValue closure)
{
    if (!lhat_is_object_kind(closure, LHAT_OBJECT_SUBROUTINE)) {
        return NULL;
    }
    return ((const LhatClosure *)lhat_as_object(closure))->proto;
}

size_t lhat_closure_capture_count(LhatValue closure)
{
    if (!lhat_is_object_kind(closure, LHAT_OBJECT_SUBROUTINE)) {
        return 0;
    }
    return ((const LhatClosure *)lhat_as_object(closure))->upvalue_count;
}

LhatValue lhat_closure_capture(LhatValue closure, size_t index)
{
    if (!lhat_is_object_kind(closure, LHAT_OBJECT_SUBROUTINE)) {
        return lhat_nil();
    }
    const LhatClosure *held = (const LhatClosure *)lhat_as_object(closure);
    if (index >= held->upvalue_count || held->upvalues[index] == NULL) {
        return lhat_nil();
    }
    return lhat_ref_get(held->upvalues[index]->location);
}

const void *lhat_closure_capture_id(LhatValue closure, size_t index)
{
    if (!lhat_is_object_kind(closure, LHAT_OBJECT_SUBROUTINE)) {
        return NULL;
    }
    const LhatClosure *held = (const LhatClosure *)lhat_as_object(closure);
    return index < held->upvalue_count ? held->upvalues[index] : NULL;
}

bool lhat_machine_make_error(LhatMachine *machine, const LhatErrorKind *kind,
                             const char *message, LhatValue cause,
                             LhatValue *out)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || kind == NULL) {
        return false;
    }
    // Same shape LHAT_BC_NEWERROR builds, and the same field defaults
    // compile_error_new writes for what a construction left out (04 の
    // 2.3): every kind has message and cause without either being declared.
    LhatError *error = lhat_error_new(&m->objects, kind);
    if (error == NULL) {
        return false;
    }
    LhatString *message_key = lhat_string_new(&m->objects, "message", 7);
    LhatString *message_text = lhat_string_new(
        &m->objects, message != NULL ? message : "",
        message != NULL ? strlen(message) : 0);
    if (message_key == NULL || message_text == NULL) {
        return false;
    }
    bool refused = false;
    if (!set_key(m, error->fields, lhat_object((LhatObject *)message_key),
                 lhat_object((LhatObject *)message_text), &refused) ||
        refused) {
        return false;
    }
    if (!lhat_is_nil(cause)) {
        LhatString *cause_key = lhat_string_new(&m->objects, "cause", 5);
        if (cause_key == NULL) {
            return false;
        }
        if (!set_key(m, error->fields, lhat_object((LhatObject *)cause_key),
                     cause, &refused) ||
            refused) {
            return false;
        }
    }
    *out = lhat_object((LhatObject *)error);
    return true;
}

const char *lhat_run_status_message(LhatRunStatus status)
{
    switch (status) {
        case LHAT_RUN_OK:              return "ran";
        case LHAT_RUN_TYPE_ERROR:      return "an instruction was given the wrong type";
        case LHAT_RUN_NOT_CALLABLE:    return "this is not a subroutine";
        case LHAT_RUN_ARITY:           return "the wrong number of arguments";
        case LHAT_RUN_STACK_OVERFLOW:  return "the calls went too deep";
        case LHAT_RUN_OUT_OF_MEMORY:   return "out of memory";
        case LHAT_RUN_BAD_KEY:         return "this cannot be a key";
        case LHAT_RUN_SEALED:
            return "this table belongs to the machine; what it holds is "
                   "written by the host, not from here";
        // 02 の 14.11
        case LHAT_RUN_MUTABLE_DEFAULT:
            return "a field's default lives on the prototype: an immutable "
                   "value, a table of its own (each instance is given a "
                   "copy), or a definition; what something else made is "
                   "given inside new";
        case LHAT_RUN_BAD_FORMAT:
            return "a number^ is written through one numeric conversion; "
                   "write '%d' or '%g' and no length of your own";
        case LHAT_RUN_DEAD_COROUTINE:  return "this coroutine has finished";
        case LHAT_RUN_NO_SUCH_UNIT:
            return "this machine was not given the unit this require^ asks for";
        case LHAT_RUN_YIELD_OUTSIDE:   return "nothing is waiting for this yield^";
        case LHAT_RUN_NO_CANDIDATE:    return "no way of calling this member takes these arguments";
        case LHAT_RUN_COROUTINE_NOT_STARTED:     return "resume needs start() first";
        case LHAT_RUN_COROUTINE_ALREADY_STARTED: return "start() only works before the first resume";
        // 04 の 11.6: a placeholder for a caller that only wants this
        // status's own name -- the actual message is what the program
        // panicked with, in LhatRunResult.value, which this cannot see.
        case LHAT_RUN_PANIC:                     return "panic^";
        // 02 の 13.8改
        case LHAT_RUN_TUPLE_ARITY:
            return "this call and what it called disagree on how many values "
                   "come back";
        case LHAT_RUN_TUPLE_UNEXPECTED:
            return "this answered several values where one was expected; "
                   "pack^ makes a table of them";
    }
    return "unknown";
}
