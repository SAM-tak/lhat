// L^ (lhat) -- the runtime representation of a value.

#include "value.h"

#include "object.h"

bool lhat_value_equal(LhatValue a, LhatValue b)
{
    // 02 の 14.8: number^ is one type, so 1 and 1.0 are the same number even
    // though 2.2 gives them different tags. Comparing the tags first would
    // make the representation visible, which is exactly what 14.8 says it is
    // not.
    if (lhat_is_number(a) && lhat_is_number(b)) {
        if (lhat_is_integer(a) && lhat_is_integer(b)) {
            return a.as.integer == b.as.integer;
        }
        // Past 2^53 a double cannot name every integer, so the comparison is
        // done the other way round when one side is exact: the real has to
        // be a whole number that survives the trip back.
        if (lhat_is_integer(a)) {
            double other = b.as.real;
            return (double)a.as.integer == other &&
                   (int64_t)other == a.as.integer;
        }
        if (lhat_is_integer(b)) {
            double other = a.as.real;
            return (double)b.as.integer == other &&
                   (int64_t)other == b.as.integer;
        }
        return a.as.real == b.as.real;
    }

    if (a.tag != b.tag) {
        return false;
    }

    switch (a.tag) {
        case LHAT_VALUE_NIL:
            return true;
        case LHAT_VALUE_BOOL:
            return a.as.boolean == b.as.boolean;
        case LHAT_VALUE_OBJECT:
            // A string is what it says, so two of them are equal when they
            // spell the same bytes. Everything else is equal only to itself:
            // 14.2 makes a table's identity what it is, and comparing two of
            // them member by member would follow a cycle for ever.
            if (a.as.object != NULL && b.as.object != NULL &&
                a.as.object->kind == LHAT_OBJECT_STRING &&
                b.as.object->kind == LHAT_OBJECT_STRING) {
                return lhat_string_equal((const LhatString *)a.as.object,
                                         (const LhatString *)b.as.object);
            }
            return a.as.object == b.as.object;
        default:
            return false;  // the numeric tags are handled above
    }
}

const char *lhat_value_tag_name(LhatValueTag tag)
{
    switch (tag) {
        case LHAT_VALUE_NIL:     return "nil^";
        case LHAT_VALUE_BOOL:    return "bool^";
        case LHAT_VALUE_INTEGER: return "number^ (integer)";
        case LHAT_VALUE_REAL:    return "number^ (real)";
        case LHAT_VALUE_OBJECT:  return "object";
    }
    return "?";
}

const char *lhat_object_kind_name(LhatObjectKind kind)
{
    switch (kind) {
        case LHAT_OBJECT_STRING:     return "string^";
        case LHAT_OBJECT_TABLE:      return "table^";
        case LHAT_OBJECT_SUBROUTINE: return "subroutine";
        case LHAT_OBJECT_COROUTINE:  return "coroutine";
        case LHAT_OBJECT_ERROR:      return "error^";
        case LHAT_OBJECT_ERROR_KIND: return "error^ kind";
        case LHAT_OBJECT_NATIVE:     return "a runtime operation";
        case LHAT_OBJECT_UPVALUE:    return "upvalue";
    }
    return "?";
}
