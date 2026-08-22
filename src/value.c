// L^ (lhat) -- the runtime representation of a value.

#include "lhat/value.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "code.h"
#include "lhat/object.h"

bool lhat_value_close(LhatValue a, LhatValue b, double tolerance)
{
    // 14.8: only a real carries an error term. Two integers name themselves
    // exactly, and admitting a band around one would put a key, an index and
    // a count inside it -- so they go the exact way, as does everything that
    // is not a pair of numbers at all.
    if (!lhat_is_number(a) || !lhat_is_number(b) ||
        (lhat_is_integer(a) && lhat_is_integer(b))) {
        return lhat_value_equal(a, b);
    }

    double x = lhat_number_as_real(a);
    double y = lhat_number_as_real(b);
    // An infinity is this far from itself and no distance from anything else:
    // the difference below would be a NaN, which is under no bound at all.
    // A NaN reaches neither this nor the bound, which is what makes it equal
    // to nothing including itself (03 の 5.8 keeps one out of a key for that
    // very reason).
    if (x == y) {
        return true;
    }

    double apart = x > y ? x - y : y - x;
    double size = x < 0.0 ? -x : x;
    double other = y < 0.0 ? -y : y;
    if (other > size) {
        size = other;
    }
    // An infinity that is not the same infinity, or a NaN. Scaling by one
    // would make the bound infinite and take in every number there is, so the
    // two are read as apart -- which for an infinity against anything finite,
    // and for the two ends against each other, is what they are.
    if (size - size != 0.0) {
        return false;
    }
    // The floor is what lets a difference that should have cancelled reach
    // zero. Without it the bound at zero is zero, and 'a - b = 0' would be
    // the one comparison an error term never helps.
    if (size < 1.0) {
        size = 1.0;
    }
    return apart <= tolerance * size;
}

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
            // 02 の 2811: typeof^(x) = typeof^(y) compares structurally
            // (11.3, 14.9) -- two calls to typeof^ build separate objects
            // even for the same shape, so pointer identity would say no to
            // the very comparison 2811 exists for.
            if (a.as.object != NULL && b.as.object != NULL &&
                a.as.object->kind == LHAT_OBJECT_TYPE &&
                b.as.object->kind == LHAT_OBJECT_TYPE) {
                return lhat_runtime_type_equal(
                    (const LhatRuntimeType *)a.as.object,
                    (const LhatRuntimeType *)b.as.object);
            }
            // 05 の 8.9改: a box is its bytes, as the value it holds is on
            // the stack -- same tag, same bytes, whatever `sealed` says (the
            // prototype's sealed box equals its unsealed copies). is^ still
            // asks for the object itself.
            if (a.as.object != b.as.object && a.as.object != NULL &&
                b.as.object != NULL &&
                a.as.object->kind == LHAT_OBJECT_HOSTVALUE_BOX &&
                b.as.object->kind == LHAT_OBJECT_HOSTVALUE_BOX) {
                const LhatHostValueBox *left =
                    (const LhatHostValueBox *)a.as.object;
                const LhatHostValueBox *right =
                    (const LhatHostValueBox *)b.as.object;
                const LhatHostValueTag *tag = lhat_hostvalue_box_tag(left);
                return tag != NULL && tag == lhat_hostvalue_box_tag(right) &&
                       memcmp(left->run + 1, right->run + 1,
                              (tag->width - 1) * sizeof(LhatValueUnion)) == 0;
            }
            // 05 の 8.8: a hostdata stands for what the host made, so two
            // wrappers of one host object are equal -- the tag first, since
            // 7.3 is what makes the pointers comparable at all. Never after
            // a release: the pointer may have been given back and reused,
            // and a wrapper whose object is gone is equal only to itself.
            if (a.as.object != b.as.object && a.as.object != NULL &&
                b.as.object != NULL &&
                a.as.object->kind == LHAT_OBJECT_HOSTDATA &&
                b.as.object->kind == LHAT_OBJECT_HOSTDATA) {
                const LhatHostData *left = (const LhatHostData *)a.as.object;
                const LhatHostData *right = (const LhatHostData *)b.as.object;
                return left->tag != NULL && left->tag == right->tag &&
                       !left->released && !right->released &&
                       left->pointer == right->pointer;
            }
            return a.as.object == b.as.object;
        default:
            return false;  // the numeric tags are handled above
    }
}

bool lhat_value_same(LhatValue a, LhatValue b)
{
    // 13.11: the same value or not, with nothing read into it. Every other
    // question about a number goes through 14.8's one type -- '=' reads 1
    // and 1.0 as one number and admits an error term besides, a key folds
    // the real form into the integer one, tostring writes what the number
    // is. This one asks what the machine is holding, which is why the tags
    // are compared rather than the numbers: it is the only place a writer
    // can put a comparison that no rounding reaches.
    if (a.tag != b.tag) {
        return false;
    }

    switch (a.tag) {
        case LHAT_VALUE_NIL:
            return true;
        case LHAT_VALUE_BOOL:
            return a.as.boolean == b.as.boolean;
        case LHAT_VALUE_INTEGER:
            return a.as.integer == b.as.integer;
        // Whatever C's '==' answers, which is what "no rounding reaches it"
        // means: -0.0 is 0.0 and a NaN is nothing, this one included.
        case LHAT_VALUE_REAL:
            return a.as.real == b.as.real;
        case LHAT_VALUE_OBJECT:
            // 'is^' asks whether the two are the same object and nothing
            // else -- no exception for a string's bytes or a type's shape.
            return a.as.object == b.as.object;
        default:
            return false;
    }
}

const char *lhat_object_kind_name(LhatObjectKind kind)
{
    switch (kind) {
        case LHAT_OBJECT_STRING:     return "string";
        case LHAT_OBJECT_TABLE:      return "table";
        case LHAT_OBJECT_SUBROUTINE: return "subroutine";
        case LHAT_OBJECT_COROUTINE:  return "coroutine";
        case LHAT_OBJECT_ERROR:      return "error";
        case LHAT_OBJECT_ERROR_KIND: return "error kind";
        case LHAT_OBJECT_NATIVE:     return "a runtime operation";
        case LHAT_OBJECT_TYPE:       return "a type";
        case LHAT_OBJECT_OVERLOAD:   return "an overloaded member";
        case LHAT_OBJECT_UPVALUE:    return "upvalue";
        // 05 の 8.7 and 8.8: what the host wrote and what the host made.
        // Neither has anything to show past what it is.
        case LHAT_OBJECT_HOST:       return "a host subroutine";
        case LHAT_OBJECT_HOSTDATA:   return "host data";
        case LHAT_OBJECT_HOSTVALUE_BOX: return "a boxed host value";
        case LHAT_OBJECT_SCRIPT:     return "a loaded script";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Writing a value down (03 の 4 章)
// ---------------------------------------------------------------------------

// snprintf into a moving cursor. `used` is how long the whole text is, which
// grows past `capacity` once the buffer is full -- the caller asks again with
// a bigger one, the way snprintf means it to.
typedef struct {
    char *out;
    size_t capacity;
    size_t used;
} Writer;

static void put(Writer *w, const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (w->out != NULL && w->used + 1 < w->capacity) {
            w->out[w->used] = text[i];
        }
        w->used++;
    }
}

static void put_text(Writer *w, const char *text)
{
    put(w, text, strlen(text));
}

static void put_number(Writer *w, const char *format, ...)
{
    char buffer[64];
    va_list args;
    va_start(args, format);
    int written = vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    if (written > 0) {
        put(w, buffer, (size_t)written);
    }
}

// 02 の 14.8: a real is written so that it cannot be read back as an integer.
// '%g' already gives a '.' or an exponent to everything but a small whole
// number, and inf and nan are words -- so what needs the '.0' is a spelling
// made of digits and a sign alone.
static void put_real(Writer *w, double real)
{
    char buffer[64];
    int written = snprintf(buffer, sizeof buffer, "%g", real);
    if (written <= 0) {
        return;
    }
    bool whole = true;
    for (int i = 0; i < written; i++) {
        char c = buffer[i];
        if (!((c >= '0' && c <= '9') || c == '-' || c == '+')) {
            whole = false;
            break;
        }
    }
    put(w, buffer, (size_t)written);
    if (whole) {
        put_text(w, ".0");
    }
}

// 01 の 5 章 spells these in a string literal, so writing one back out spells
// them the same way.
static void put_quoted(Writer *w, const LhatString *string)
{
    put_text(w, "\"");
    for (size_t i = 0; i < string->length; i++) {
        char c = string->text[i];
        switch (c) {
            case '"':  put_text(w, "\\\""); break;
            case '\\': put_text(w, "\\\\"); break;
            case '\n': put_text(w, "\\n"); break;
            case '\t': put_text(w, "\\t"); break;
            case '\r': put_text(w, "\\r"); break;
            default:   put(w, &c, 1); break;
        }
    }
    put_text(w, "\"");
}

static void write_value(Writer *w, LhatValue value, size_t depth);

// 05 の 8.9: 'module.Name(1, 2, 3)' -- the registered fields in their
// registration order, decoded off the data run. The name alone when no
// field was registered; the bytes have no other reading.
static void write_hostvalue_content(Writer *w, const LhatHostValueTag *tag,
                                    const LhatValueUnion *data,
                                    const char *suffix, size_t depth)
{
    if (tag->module != NULL) {
        put_text(w, tag->module);
        put_text(w, ".");
    }
    put_text(w, tag->name != NULL ? tag->name : "?");
    put_text(w, suffix);
    if (tag->field_count == 0) {
        return;
    }
    put_text(w, "(");
    for (size_t i = 0; i < tag->field_count; i++) {
        if (i > 0) {
            put_text(w, ", ");
        }
        write_value(w, lhat_hostvalue_field_value(data, &tag->fields[i]),
                    depth);
    }
    put_text(w, ")");
}


// 14.14: a key written as a name is the same key written as a string, so one
// that spells an identifier is written back in the shorter form.
//
// 14.17改: a member's key keeps its hats (01 の 2.3's exception), and 01 の
// 2.3 makes a word with them a name like any other -- so 'new' is written
// back as 'new' rather than quoted. The hats have to be the tail of it, as
// the lexer reads them.
static bool spells_a_name(const LhatString *string)
{
    size_t length = string->length;
    while (length > 0 && string->text[length - 1] == '^') {
        length--;
    }
    if (length == 0) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        char c = string->text[i];
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      c == '_';
        bool digit = c >= '0' && c <= '9';
        if (!letter && !(digit && i > 0)) {
            return false;
        }
    }
    return true;
}

static void write_table(Writer *w, const LhatTable *table, size_t depth)
{
    put_text(w, "{");
    bool first = true;

    // 14 章: the sequence half in index order, then the rest. 16.3 promises
    // that order for a walk, and there is no reason to write one differently.
    for (size_t i = 0; i < table->array_count; i++) {
        put_text(w, first ? " " : ", ");
        first = false;
        write_value(w, lhat_slots_get(table->array, i), depth + 1);
    }
    for (size_t i = 0; i < table->entry_capacity; i++) {
        LhatValue key = table->entries[i].key;
        if (lhat_is_nil(key)) {
            continue;
        }
        put_text(w, first ? " " : ", ");
        first = false;
        if (lhat_is_object_kind(key, LHAT_OBJECT_STRING) &&
            spells_a_name((const LhatString *)lhat_as_object(key))) {
            const LhatString *name = (const LhatString *)lhat_as_object(key);
            put(w, name->text, name->length);
        } else {
            put_text(w, "[");
            write_value(w, key, depth + 1);
            put_text(w, "]");
        }
        // 02 の 14.14改: an element is an introduction, so '=' is the spelling.
        // ':=' is still read, but writing one out here would be showing the
        // form the chapter calls the unintended one.
        put_text(w, " = ");
        write_value(w, table->entries[i].value, depth + 1);
    }

    put_text(w, first ? "}" : " }");
}

static void write_value(Writer *w, LhatValue value, size_t depth)
{
    switch (value.tag) {
        case LHAT_VALUE_NIL:
            put_text(w, "nil^");
            return;
        case LHAT_VALUE_BOOL:
            put_text(w, value.as.boolean ? "true^" : "false^");
            return;
        case LHAT_VALUE_INTEGER:
            put_number(w, "%lld", (long long)value.as.integer);
            return;
        case LHAT_VALUE_REAL:
            // 14.8 makes one number type of two representations, so a real
            // that happens to be whole still says which one it is.
            //
            // Read off the spelling rather than the value: what has to be
            // told apart is '3' from '3.0', and a real too large to be an
            // integer is already spelled with an exponent. Asking the value
            // instead would mean casting it to an integer to compare, which
            // is undefined for exactly the ones that do not fit.
            put_real(w, value.as.real);
            return;
        case LHAT_VALUE_HOSTVALUE:
            // 05 の 8.9: outside the stack an LhatValue host value is the
            // pointer form -- a host's argument aiming at the run (the head
            // form never leaves the machine; vm.c's tostring writes its
            // name itself). The spelling is the type's name over the field
            // values, read straight off the bytes. A library that wants its
            // values written its own way registers a tostring, which 14.17
            // lets win over this.
            if (value.as.hostvalue_run != NULL &&
                value.as.hostvalue_run[0].hostvalue != NULL) {
                const LhatValueUnion *run = value.as.hostvalue_run;
                write_hostvalue_content(w, run[0].hostvalue, run + 1, "",
                                        depth + 1);
                return;
            }
            put_text(w, "<hostvalue>");
            return;
        case LHAT_VALUE_CONT:
        case LHAT_VALUE_RUN:
            // Neither is ever a value in its own right: 8.9's continuation
            // slots are read through their head, and 13.8改 confines a run
            // to a result the next few instructions take apart.
            put_text(w, "<");
            put_text(w, value.tag == LHAT_VALUE_CONT ? "cont" : "run");
            put_text(w, ">");
            return;
        case LHAT_VALUE_OBJECT:
            break;
    }

    const LhatObject *object = value.as.object;
    if (object == NULL) {
        put_text(w, "nil^");
        return;
    }

    switch (object->kind) {
        case LHAT_OBJECT_STRING:
            put_quoted(w, (const LhatString *)object);
            return;
        case LHAT_OBJECT_TABLE:
            if (depth >= LHAT_WRITE_MAX_DEPTH) {
                put_text(w, "{ … }");
                return;
            }
            write_table(w, (const LhatTable *)object, depth);
            return;
        case LHAT_OBJECT_SUBROUTINE: {
            const LhatClosure *closure = (const LhatClosure *)object;
            put_text(w, closure->proto != NULL && closure->proto->is_function
                            ? "f^" : "p^");
            return;
        }
        case LHAT_OBJECT_COROUTINE:
            put_text(w, "c^");
            return;
        case LHAT_OBJECT_ERROR: {
            // 04 の 2.3: a kind plus the fields the construction gave it.
            const LhatError *error = (const LhatError *)object;
            put_text(w, "error^");
            if (error->kind != NULL && error->kind->name != NULL) {
                put(w, error->kind->name->text, error->kind->name->length);
            }
            if (error->fields != NULL) {
                if (depth >= LHAT_WRITE_MAX_DEPTH) {
                    put_text(w, "{ … }");
                } else {
                    write_table(w, error->fields, depth);
                }
            }
            return;
        }
        case LHAT_OBJECT_ERROR_KIND: {
            const LhatErrorKind *kind = (const LhatErrorKind *)object;
            put_text(w, "error^");
            if (kind->name != NULL) {
                put(w, kind->name->text, kind->name->length);
            }
            return;
        }
        // 05 の 8.9: the box written as what it holds -- its type's name,
        // which flavour of box it is, and the field values off the bytes.
        case LHAT_OBJECT_HOSTVALUE_BOX: {
            const LhatHostValueBox *box = (const LhatHostValueBox *)object;
            const LhatHostValueTag *tag = lhat_hostvalue_box_tag(box);
            if (tag == NULL) {
                put_text(w, "?.Box^");
                return;
            }
            write_hostvalue_content(w, tag, box->run + 1,
                                    box->sealed ? ".ConstBox^" : ".Box^",
                                    depth + 1);
            return;
        }
        default:
            // 05 の 8.5's natives, 14.12's overload groups and 13.11's runtime
            // types are reached through something else and never stand alone
            // as an answer, so naming the kind is as much as is wanted.
            put_text(w, "<");
            put_text(w, lhat_object_kind_name(object->kind));
            put_text(w, ">");
            return;
    }
}

size_t lhat_value_write(LhatValue value, char *out, size_t capacity)
{
    Writer w;
    w.out = out;
    w.capacity = capacity;
    w.used = 0;
    write_value(&w, value, 0);
    if (out != NULL && capacity > 0) {
        out[w.used < capacity ? w.used : capacity - 1] = '\0';
    }
    return w.used;
}

// ---------------------------------------------------------------------------
// tostring (02 の 14.17)
// ---------------------------------------------------------------------------

size_t lhat_value_text(LhatValue value, char *out, size_t capacity)
{
    Writer w;
    w.out = out;
    w.capacity = capacity;
    w.used = 0;

    // 14.17: the text a reader reads, where lhat_value_write gives the
    // spelling L^ would read back. A string^ is already its own text; nil^
    // and bool^ are words rather than the hatted literals that name them.
    // Only the value itself is read this way -- one inside a table is
    // written as the table spells it, which is the table's spelling and not
    // that value's.
    if (lhat_is_object_kind(value, LHAT_OBJECT_STRING)) {
        const LhatString *string = (const LhatString *)lhat_as_object(value);
        put(&w, string->text, string->length);
    } else if (lhat_is_nil(value)) {
        put_text(&w, "nil");
    } else if (lhat_is_bool(value)) {
        put_text(&w, lhat_as_bool(value) ? "true" : "false");
    } else {
        return lhat_value_write(value, out, capacity);
    }

    if (out != NULL && capacity > 0) {
        out[w.used < capacity ? w.used : capacity - 1] = '\0';
    }
    return w.used;
}

// Which reading of a number^ a conversion asks for, or that it asks for
// something a number^ cannot answer.
typedef enum {
    FORMAT_BAD,
    FORMAT_INTEGER,
    FORMAT_REAL
} FormatFamily;

// Reads the written format and builds the one to hand snprintf. They differ
// only in the length modifier, which the writer does not spell and this
// supplies -- see lhat_number_format's comment in value.h.
static FormatFamily read_format(const char *format, size_t length,
                                char *rebuilt, size_t room)
{
    size_t out = 0;
    FormatFamily family = FORMAT_BAD;  // still nothing to write the number into

    for (size_t at = 0; at < length;) {
        char c = format[at];
        if (c == '\0') {
            // snprintf would stop here and the rest would be a lie, so this
            // is not a format even though the bytes are there.
            return FORMAT_BAD;
        }
        if (c != '%') {
            if (out + 1 >= room) {
                return FORMAT_BAD;
            }
            rebuilt[out++] = c;
            at++;
            continue;
        }
        // '%%' is a per cent sign the writer wanted, not a conversion.
        if (at + 1 < length && format[at + 1] == '%') {
            if (out + 2 >= room) {
                return FORMAT_BAD;
            }
            rebuilt[out++] = '%';
            rebuilt[out++] = '%';
            at += 2;
            continue;
        }
        if (family != FORMAT_BAD || out + 1 >= room) {
            return FORMAT_BAD;  // a second conversion, with one number to write
        }
        rebuilt[out++] = '%';
        at++;

        // Flags, width and precision. '*' is refused: it reads its number
        // from a further argument, and there is only ever the one here.
        while (at < length) {
            char f = format[at];
            bool carried = (f >= '0' && f <= '9') || f == '-' || f == '+' ||
                           f == ' ' || f == '#' || f == '.';
            if (!carried) {
                break;
            }
            if (out + 1 >= room) {
                return FORMAT_BAD;
            }
            rebuilt[out++] = f;
            at++;
        }
        if (at >= length) {
            return FORMAT_BAD;  // the conversion was never finished
        }

        char spec = format[at++];
        switch (spec) {
            case 'd': case 'i': case 'o': case 'u': case 'x': case 'X':
                family = FORMAT_INTEGER;
                if (out + 2 >= room) {
                    return FORMAT_BAD;
                }
                rebuilt[out++] = 'l';
                rebuilt[out++] = 'l';
                break;
            case 'f': case 'F': case 'e': case 'E':
            case 'g': case 'G': case 'a': case 'A':
                family = FORMAT_REAL;  // a double is what varargs promote to
                break;
            default:
                // 's' and 'p' would read the number as an address and 'n'
                // would write through one. A length modifier lands here too:
                // naming a width is not the writer's to do.
                return FORMAT_BAD;
        }
        if (out + 1 >= room) {
            return FORMAT_BAD;
        }
        rebuilt[out++] = spec;
        // Whatever the writer put after it is carried over by the loop, and
        // a second conversion among it is refused where this one was taken.
    }

    rebuilt[out] = '\0';
    return family;  // FORMAT_BAD when nothing asked for the number
}

bool lhat_number_format(LhatValue value, const char *format,
                        size_t format_length, char *out, size_t capacity,
                        size_t *needed)
{
    char rebuilt[LHAT_FORMAT_MAX_BYTES];
    FormatFamily family =
        read_format(format, format_length, rebuilt, sizeof rebuilt);
    if (family == FORMAT_BAD) {
        return false;
    }

    int written;
    if (family == FORMAT_INTEGER) {
        int64_t whole;
        if (lhat_is_integer(value)) {
            whole = lhat_as_integer(value);
        } else {
            // 14.8: the conversion decides the reading, so a real asked for
            // as an integer is read as one -- but only where it names one.
            // Outside that range the conversion is undefined in C, which is
            // not an answer this may give.
            double real = lhat_as_real(value);
            if (!(real >= -9223372036854775808.0 &&
                  real < 9223372036854775808.0)) {
                return false;
            }
            whole = (int64_t)real;
        }
        written = snprintf(out, out != NULL ? capacity : 0, rebuilt,
                           (long long)whole);
    } else {
        written = snprintf(out, out != NULL ? capacity : 0, rebuilt,
                           lhat_number_as_real(value));
    }
    if (written < 0) {
        return false;
    }
    *needed = (size_t)written;
    return true;
}

// read_format's sibling, for sscanf rather than snprintf. Beside it rather
// than folded into it: the two agree on which conversions a number^ may go
// through and on there being exactly one of them, and disagree on everything
// around it -- the length modifier a double wants, the flags, the precision.
// One function taking a flag would be two functions with a seam down it.
//
// The rebuilt format ends in "%n" so that the caller can demand the whole
// text was read. %n is not counted by sscanf's answer, which is why the
// caller checks both.
static FormatFamily read_scan_format(const char *format, size_t length,
                                     char *rebuilt, size_t room,
                                     char *conversion)
{
    *conversion = '\0';
    size_t out = 0;
    FormatFamily family = FORMAT_BAD;  // still nothing to read the number into

    for (size_t at = 0; at < length;) {
        char c = format[at];
        if (c == '\0') {
            // sscanf would stop here and the rest would be a lie, so this is
            // not a format even though the bytes are there.
            return FORMAT_BAD;
        }
        if (c != '%') {
            if (out + 1 >= room) {
                return FORMAT_BAD;
            }
            rebuilt[out++] = c;
            at++;
            continue;
        }
        if (at + 1 < length && format[at + 1] == '%') {
            if (out + 2 >= room) {
                return FORMAT_BAD;
            }
            rebuilt[out++] = '%';
            rebuilt[out++] = '%';
            at += 2;
            continue;
        }
        if (family != FORMAT_BAD || out + 1 >= room) {
            return FORMAT_BAD;  // a second conversion, with one number to read
        }
        rebuilt[out++] = '%';
        at++;

        // Width only. '*' would suppress the assignment and leave the number
        // unwritten; '.' and the printf flags are not scanf's, so a format
        // carrying them was never one this could read through.
        while (at < length && format[at] >= '0' && format[at] <= '9') {
            if (out + 1 >= room) {
                return FORMAT_BAD;
            }
            rebuilt[out++] = format[at++];
        }
        if (at >= length) {
            return FORMAT_BAD;  // the conversion was never finished
        }

        char spec = format[at++];
        switch (spec) {
            case 'd': case 'i': case 'o': case 'u': case 'x': case 'X':
                family = FORMAT_INTEGER;
                if (out + 2 >= room) {
                    return FORMAT_BAD;
                }
                rebuilt[out++] = 'l';
                rebuilt[out++] = 'l';
                break;
            case 'f': case 'F': case 'e': case 'E':
            case 'g': case 'G': case 'a': case 'A':
                family = FORMAT_REAL;
                // Where printf promotes a float to a double and wants no
                // modifier, scanf writes through a pointer and has to be told
                // which one. The value is a double, so 'l' it is.
                if (out + 1 >= room) {
                    return FORMAT_BAD;
                }
                rebuilt[out++] = 'l';
                break;
            default:
                // 's', 'c' and '[' would read text, 'p' an address and 'n' a
                // count, and none of those is a number^. A length modifier
                // lands here too: naming a width is not the writer's to do.
                return FORMAT_BAD;
        }
        if (out + 1 >= room) {
            return FORMAT_BAD;
        }
        rebuilt[out++] = spec;
        *conversion = spec;
    }

    if (family == FORMAT_BAD || out + 3 >= room) {
        return FORMAT_BAD;  // nothing asked for the number, or no room for %n
    }
    rebuilt[out++] = '%';
    rebuilt[out++] = 'n';
    rebuilt[out] = '\0';
    return family;
}

bool lhat_number_scan(const char *text, size_t length, const char *format,
                      size_t format_length, LhatValue *out, bool *read)
{
    char rebuilt[LHAT_FORMAT_MAX_BYTES];
    char conversion = '\0';
    FormatFamily family = read_scan_format(format, format_length, rebuilt,
                                           sizeof rebuilt, &conversion);
    if (family == FORMAT_BAD) {
        return false;
    }

    // A count sscanf can always hold: the text may be longer than an int can
    // say, and one that long was never going to be a single number anyway.
    if (length > (size_t)INT_MAX) {
        *read = false;
        return true;
    }

    // 'o', 'u', 'x' and 'X' write through an unsigned long long and the other
    // two through a signed one. The pointer has to be the one the conversion
    // names, so the two are kept apart here rather than cast together.
    bool unsigned_read = conversion == 'o' || conversion == 'u' ||
                         conversion == 'x' || conversion == 'X';
    int consumed = -1;
    *read = false;
    if (family == FORMAT_REAL) {
        double real = 0.0;
        if (sscanf(text, rebuilt, &real, &consumed) == 1 &&
            consumed == (int)length) {
            *out = lhat_real(real);
            *read = true;
        }
    } else if (unsigned_read) {
        unsigned long long magnitude = 0;
        // A number^ is an int64_t (14.8), so one written past its range names
        // no number^ this can answer with.
        if (sscanf(text, rebuilt, &magnitude, &consumed) == 1 &&
            consumed == (int)length &&
            magnitude <= (unsigned long long)INT64_MAX) {
            *out = lhat_integer((int64_t)magnitude);
            *read = true;
        }
    } else {
        long long whole = 0;
        if (sscanf(text, rebuilt, &whole, &consumed) == 1 &&
            consumed == (int)length) {
            *out = lhat_integer((int64_t)whole);
            *read = true;
        }
    }
    // Whatever is left is a text that did not match, or one that matched a
    // prefix and left bytes over -- a NUL among them stops sscanf and lands
    // here as well.
    return true;
}
