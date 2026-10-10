// L^ (lhat) -- sample standard library: std.binary (11 章).
//
// A kind (std.binary.Kind) is a width and a rule for turning a value into
// that many bits; a format (std.binary.Format, a Kind itself) is an ordered
// list of named kinds. Both are hostdata holding a tree of their own: every
// kind handed to array() or format() is copied in, so no tree reaches into
// another and each is freed whole by its own dispose^.
//
// Bits go in from the low end of each byte, and a value spanning bytes puts
// its low byte first (11 の 3.4). A table handed to encode that does not fit
// the format is the writer's mistake and panics; bytes handed to decode came
// from outside and answer std.binary.Error (11 の 3.5).

#include "binary.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    KIND_UINT,
    KIND_INT,
    KIND_BOOL,
    KIND_RANGE,
    KIND_FIXED,
    KIND_ENUM,
    KIND_ARRAY,
    KIND_FORMAT
} KindTag;

typedef struct Kind Kind;

typedef struct {
    char *name;
    size_t length;
    Kind *kind;
} Field;

struct Kind {
    KindTag tag;
    uint64_t bits;   // the whole width, an array's or a format's included
    unsigned width;  // one scalar's bits
    // UINT, RANGE, FIXED, ENUM: the largest number packed, the smallest
    // being 0. RANGE adds `low` back; FIXED multiplies by `step` from
    // `origin`.
    uint64_t span;
    int64_t low;
    double origin;
    double step;
    // ENUM: the members by their 0-based place. The enum is rooted on the
    // machine that made this kind (lhat_machine_host_root), which keeps them.
    const LhatEnum *owner;
    const LhatEnumerator **members;
    // ARRAY
    size_t length;
    Kind *element;
    // FORMAT
    Field *fields;
    size_t field_count;
};

// The largest format is 2^32 bits: what one datagram or one string holds is
// far below that, and it keeps every count below in range.
#define MAX_BITS ((uint64_t)1 << 32)

typedef struct {
    const LhatHostDataTag *kind_tag;
    const LhatHostDataTag *format_tag;
    const LhatHostDataTag *bytes_tag;
    const LhatErrorKind *truncated;
    const LhatErrorKind *malformed;
} BinaryModule;

// 05 の 8.7: the tags and kinds are the process's, so one of these serves
// every program that registers the module.
static BinaryModule shared;

static LhatBinaryBytes *bytes_of(LhatValue value);
static bool bytes_resize(LhatBinaryBytes *bytes, size_t length);

// ---------------------------------------------------------------------------
// Kinds

static void kind_free(Kind *kind)
{
    if (kind == NULL) {
        return;
    }
    kind_free(kind->element);
    for (size_t i = 0; i < kind->field_count; i++) {
        lhat_free(kind->fields[i].name);
        kind_free(kind->fields[i].kind);
    }
    lhat_free(kind->fields);
    lhat_free((void *)kind->members);
    lhat_free(kind);
}

static Kind *kind_copy(const Kind *from)
{
    Kind *kind = (Kind *)lhat_alloc(sizeof *kind);
    if (kind == NULL) {
        return NULL;
    }
    *kind = *from;
    kind->element = NULL;
    kind->fields = NULL;
    kind->field_count = 0;
    kind->members = NULL;
    bool ok = true;
    if (from->members != NULL) {
        size_t size = (size_t)(from->span + 1) * sizeof *kind->members;
        kind->members = (const LhatEnumerator **)lhat_alloc(size);
        ok = kind->members != NULL;
        if (ok) memcpy((void *)kind->members, from->members, size);
    }
    if (ok && from->element != NULL) {
        kind->element = kind_copy(from->element);
        ok = kind->element != NULL;
    }
    if (ok && from->field_count > 0) {
        kind->fields = (Field *)lhat_calloc(from->field_count, sizeof *kind->fields);
        ok = kind->fields != NULL;
        for (size_t i = 0; ok && i < from->field_count; i++) {
            kind->field_count = i + 1;
            Field *field = &kind->fields[i];
            field->length = from->fields[i].length;
            field->name = (char *)lhat_alloc(field->length + 1);
            field->kind = kind_copy(from->fields[i].kind);
            ok = field->name != NULL && field->kind != NULL;
            if (field->name != NULL) {
                memcpy(field->name, from->fields[i].name, field->length + 1);
            }
        }
    }
    if (!ok) {
        kind_free(kind);
        return NULL;
    }
    return kind;
}

// How many bits the numbers 0..span take.
static unsigned width_of(uint64_t span)
{
    unsigned width = 0;
    while (span != 0) {
        width++;
        span >>= 1;
    }
    return width;
}

// The pointer behind a value of `tag`'s type, or NULL when it is not one or
// has been given back already.
static void *live(LhatValue value, const LhatHostDataTag *tag)
{
    void *pointer = lhat_hostdata_pointer(value, tag);
    return pointer != NULL && !((const LhatHostData *)lhat_as_object(value))->released
               ? pointer
               : NULL;
}

static void panic_with(LhatMachine *machine, const char *format, const char *a,
                       const char *b)
{
    char text[256];
    snprintf(text, sizeof text, format, a, b);
    lhat_machine_panic_text(machine, text);
}

static void out_of_memory(LhatMachine *machine)
{
    lhat_machine_panic_text(machine, "out of memory");
}

// Hands `kind` to L^ as a Kind, or as a Format when it is one.
static void answer_kind(LhatMachine *machine, Kind *kind, LhatValue *answers,
                        int *answer_count)
{
    if (kind == NULL) {
        out_of_memory(machine);
        return;
    }
    const LhatHostDataTag *tag =
        kind->tag == KIND_FORMAT ? shared.format_tag : shared.kind_tag;
    LhatValue out = lhat_nil();
    if (!lhat_machine_make_hostdata(machine, tag, kind, &out)) {
        kind_free(kind);
        out_of_memory(machine);
        return;
    }
    answers[0] = out;
    *answer_count = 1;
}

static Kind *new_kind(KindTag tag, unsigned width, uint64_t span)
{
    Kind *kind = (Kind *)lhat_calloc(1, sizeof *kind);
    if (kind != NULL) {
        kind->tag = tag;
        kind->width = width;
        kind->bits = width;
        kind->span = span;
    }
    return kind;
}

// An argument that has to be an integer in [low, high], or a panic naming
// the function it was handed to.
static bool integer_argument(LhatMachine *machine, LhatValue value, int64_t low,
                             int64_t high, const char *function, int64_t *out)
{
    if (!lhat_is_integer(value) || lhat_as_integer(value) < low ||
        lhat_as_integer(value) > high) {
        char text[128];
        snprintf(text, sizeof text, "std.binary.%s: expected an integer from %lld to %lld",
                 function, (long long)low, (long long)high);
        lhat_machine_panic_text(machine, text);
        return false;
    }
    *out = lhat_as_integer(value);
    return true;
}

static void binary_uint(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count,
                        LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    int64_t n = 0;
    if (integer_argument(machine, arguments[0], 1, 64, "uint", &n)) {
        uint64_t span = n == 64 ? UINT64_MAX : ((uint64_t)1 << n) - 1;
        answer_kind(machine, new_kind(KIND_UINT, (unsigned)n, span), answers,
                    answer_count);
    }
}

static void binary_int(LhatMachine *machine, void *context,
                       const LhatValue *arguments, size_t count,
                       LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    int64_t n = 0;
    if (integer_argument(machine, arguments[0], 1, 64, "int", &n)) {
        answer_kind(machine, new_kind(KIND_INT, (unsigned)n, 0), answers,
                    answer_count);
    }
}

static void binary_bool(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count,
                        LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)arguments;
    (void)count;
    answer_kind(machine, new_kind(KIND_BOOL, 1, 1), answers, answer_count);
}

static void binary_range(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count,
                         LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    int64_t low = 0;
    int64_t high = 0;
    if (!integer_argument(machine, arguments[0], INT64_MIN, INT64_MAX, "range", &low) ||
        !integer_argument(machine, arguments[1], low, INT64_MAX, "range", &high)) {
        return;
    }
    uint64_t span = (uint64_t)high - (uint64_t)low;
    Kind *kind = new_kind(KIND_RANGE, width_of(span), span);
    if (kind != NULL) kind->low = low;
    answer_kind(machine, kind, answers, answer_count);
}

static void binary_fixed(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count,
                         LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    double low = lhat_number_as_real(arguments[0]);
    double high = lhat_number_as_real(arguments[1]);
    double step = lhat_number_as_real(arguments[2]);
    double steps = (high - low) / step;
    // 2^53: past it the steps are no longer whole numbers a double can count.
    if (!(step > 0) || !(high >= low) || !(steps < 9007199254740992.0)) {
        lhat_machine_panic_text(
            machine, "std.binary.fixed: expected min <= max and a step above 0");
        return;
    }
    uint64_t span = (uint64_t)llround(steps);
    Kind *kind = new_kind(KIND_FIXED, width_of(span), span);
    if (kind != NULL) {
        kind->origin = low;
        kind->step = step;
    }
    answer_kind(machine, kind, answers, answer_count);
}

static void binary_enum(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count,
                        LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    if (!lhat_is_object_kind(arguments[0], LHAT_OBJECT_ENUM)) {
        lhat_machine_panic_text(machine, "std.binary.enum: expected an enum^");
        return;
    }
    const LhatEnum *owner = (const LhatEnum *)lhat_as_object(arguments[0]);
    size_t members = lhat_table_count(owner->members);
    if (members == 0) {
        lhat_machine_panic_text(machine, "std.binary.enum: the enum^ has no members");
        return;
    }
    // A kind is no root, so the enum it reads its members from is made one.
    bool refused = false;
    if (!lhat_machine_table_set(machine, lhat_machine_host_root(machine),
                                arguments[0], lhat_bool(true), &refused)) {
        out_of_memory(machine);
        return;
    }
    Kind *kind = new_kind(KIND_ENUM, width_of(members - 1), members - 1);
    if (kind != NULL) {
        kind->owner = owner;
        kind->members = (const LhatEnumerator **)lhat_calloc(members, sizeof *kind->members);
        if (kind->members == NULL) {
            kind_free(kind);
            kind = NULL;
        }
    }
    if (kind != NULL) {
        LhatWalkCursor walk = {owner->members, 0, 0, LHAT_WALK_PAIR};
        LhatValue key = lhat_nil();
        LhatValue value = lhat_nil();
        while (lhat_table_walk(&walk, &key, &value)) {
            if (lhat_is_object_kind(value, LHAT_OBJECT_ENUMERATOR)) {
                const LhatEnumerator *member = (const LhatEnumerator *)lhat_as_object(value);
                if (member->index >= 1 && member->index <= members) {
                    kind->members[member->index - 1] = member;
                }
            }
        }
    }
    answer_kind(machine, kind, answers, answer_count);
}

static void binary_array(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count,
                         LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    int64_t length = 0;
    const Kind *element = (const Kind *)live(arguments[1], shared.kind_tag);
    if (!integer_argument(machine, arguments[0], 0, (int64_t)MAX_BITS, "array", &length)) {
        return;
    }
    if (element == NULL) {
        lhat_machine_panic_text(machine, "std.binary.array: the kind was disposed");
        return;
    }
    if (length > 0 && element->bits > MAX_BITS / (uint64_t)length) {
        lhat_machine_panic_text(machine, "std.binary.array: too many bits");
        return;
    }
    Kind *kind = new_kind(KIND_ARRAY, 0, 0);
    if (kind != NULL) {
        kind->bits = element->bits * (uint64_t)length;
        kind->length = (size_t)length;
        kind->element = kind_copy(element);
        if (kind->element == NULL) {
            kind_free(kind);
            kind = NULL;
        }
    }
    answer_kind(machine, kind, answers, answer_count);
}

static const LhatString *as_string(LhatValue value)
{
    return lhat_is_object_kind(value, LHAT_OBJECT_STRING)
               ? (const LhatString *)lhat_as_object(value)
               : NULL;
}

static LhatTable *as_table(LhatValue value)
{
    return lhat_is_object_kind(value, LHAT_OBJECT_TABLE)
               ? (LhatTable *)lhat_as_object(value)
               : NULL;
}

// 11 の 3.1: the rows are read once, here, and every mistake in them is the
// writer's.
static void binary_format(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count,
                          LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    const LhatTable *rows = as_table(arguments[0]);
    size_t length = rows != NULL ? lhat_table_length(rows) : 0;
    Kind *format = new_kind(KIND_FORMAT, 0, 0);
    if (format != NULL && length > 0) {
        format->fields = (Field *)lhat_calloc(length, sizeof *format->fields);
        if (format->fields == NULL) {
            kind_free(format);
            format = NULL;
        }
    }
    if (format == NULL) {
        out_of_memory(machine);
        return;
    }
    for (size_t i = 0; i < length; i++) {
        const LhatTable *row = as_table(lhat_table_get(rows, lhat_integer((int64_t)i)));
        const LhatString *name =
            row != NULL ? as_string(lhat_table_get(row, lhat_integer(0))) : NULL;
        const Kind *kind =
            row != NULL ? (const Kind *)live(lhat_table_get(row, lhat_integer(1)),
                                             shared.kind_tag)
                        : NULL;
        const char *problem = NULL;
        if (name == NULL || kind == NULL) {
            problem = "std.binary.format: row %s is not { name, kind }%s";
        } else {
            for (size_t j = 0; j < i && problem == NULL; j++) {
                if (format->fields[j].length == name->length &&
                    memcmp(format->fields[j].name, name->text, name->length) == 0) {
                    problem = "std.binary.format: the name %s is written twice%s";
                }
            }
            if (problem == NULL && format->bits + kind->bits > MAX_BITS) {
                problem = "std.binary.format: too many bits at %s%s";
            }
        }
        if (problem != NULL) {
            char at[64];
            if (name != NULL && kind != NULL) {
                snprintf(at, sizeof at, "%.*s", (int)(name->length < 48 ? name->length : 48),
                         name->text);
            } else {
                snprintf(at, sizeof at, "%zu", i);
            }
            kind_free(format);
            panic_with(machine, problem, at, "");
            return;
        }
        Field *field = &format->fields[i];
        format->field_count = i + 1;
        field->length = name->length;
        field->name = (char *)lhat_alloc(name->length + 1);
        field->kind = kind_copy(kind);
        if (field->name == NULL || field->kind == NULL) {
            kind_free(format);
            out_of_memory(machine);
            return;
        }
        memcpy(field->name, name->text, name->length);
        field->name[name->length] = '\0';
        format->bits += kind->bits;
    }
    answer_kind(machine, format, answers, answer_count);
}

static void kind_bits(LhatMachine *machine, void *context,
                      const LhatValue *arguments, size_t count,
                      LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    const Kind *kind = (const Kind *)live(arguments[0], shared.kind_tag);
    if (kind == NULL) {
        lhat_machine_panic_text(machine, "std.binary: the kind was disposed");
        return;
    }
    answers[0] = lhat_integer((int64_t)kind->bits);
    *answer_count = 1;
}

static void format_size(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count,
                        LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    const Kind *kind = (const Kind *)live(arguments[0], shared.format_tag);
    if (kind == NULL) {
        lhat_machine_panic_text(machine, "std.binary: the format was disposed");
        return;
    }
    answers[0] = lhat_integer((int64_t)((kind->bits + 7) / 8));
    *answer_count = 1;
}

static void kind_dispose(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count,
                         LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    (void)count;
    (void)answers;
    (void)answer_count;
    kind_free((Kind *)lhat_hostdata_pointer(arguments[0], shared.kind_tag));
}

// ---------------------------------------------------------------------------
// Bits

static void put_bits(uint8_t *out, uint64_t *at, uint64_t value, unsigned width)
{
    uint64_t p = *at;
    while (width > 0) {
        unsigned offset = (unsigned)(p & 7);
        unsigned take = 8 - offset < width ? 8 - offset : width;
        out[p >> 3] |= (uint8_t)((value & ((1u << take) - 1)) << offset);
        value >>= take;
        p += take;
        width -= take;
    }
    *at = p;
}

static uint64_t get_bits(const uint8_t *in, uint64_t *at, unsigned width)
{
    uint64_t p = *at;
    uint64_t value = 0;
    unsigned got = 0;
    while (got < width) {
        unsigned offset = (unsigned)(p & 7);
        unsigned take = 8 - offset < width - got ? 8 - offset : width - got;
        value |= (uint64_t)((in[p >> 3] >> offset) & ((1u << take) - 1)) << got;
        p += take;
        got += take;
    }
    *at = p;
    return value;
}

// ---------------------------------------------------------------------------
// Encoding

// Where a value sits, for a panic to name: the field, and an array position
// under it when there is one.
typedef struct {
    const Field *field;
    int64_t index;
} Place;

static bool refuse(LhatMachine *machine, Place place, const char *why)
{
    char at[96];
    if (place.field == NULL) {
        snprintf(at, sizeof at, "the value");
    } else if (place.index >= 0) {
        snprintf(at, sizeof at, "%.*s[%lld]", (int)(place.field->length < 64 ? place.field->length : 64),
                 place.field->name, (long long)place.index);
    } else {
        snprintf(at, sizeof at, "%.*s", (int)(place.field->length < 64 ? place.field->length : 64),
                 place.field->name);
    }
    panic_with(machine, "std.binary.encode: %s %s", at, why);
    return false;
}

static bool encode_kind(LhatMachine *machine, const Kind *kind, LhatValue value,
                        uint8_t *out, uint64_t *at, Place place)
{
    switch (kind->tag) {
    case KIND_UINT:
    case KIND_RANGE:
    case KIND_INT: {
        if (!lhat_is_integer(value)) {
            return refuse(machine, place, "is not an integer");
        }
        int64_t n = lhat_as_integer(value);
        uint64_t packed;
        if (kind->tag == KIND_INT) {
            int64_t half = kind->width == 64 ? INT64_MIN : -((int64_t)1 << (kind->width - 1));
            if (n < half || (kind->width < 64 && n > -half - 1)) {
                return refuse(machine, place, "does not fit int");
            }
            packed = (uint64_t)n;
        } else {
            packed = (uint64_t)n - (uint64_t)kind->low;
            bool below = kind->tag == KIND_UINT ? n < 0 : n < kind->low;
            if (below || packed > kind->span) {
                return refuse(machine, place, kind->tag == KIND_UINT ? "does not fit uint"
                                                                      : "is out of range");
            }
        }
        put_bits(out, at, packed, kind->width);
        return true;
    }
    case KIND_BOOL:
        if (!lhat_is_bool(value)) {
            return refuse(machine, place, "is not a bool^");
        }
        put_bits(out, at, lhat_as_bool(value) ? 1 : 0, 1);
        return true;
    case KIND_FIXED: {
        if (!lhat_is_number(value)) {
            return refuse(machine, place, "is not a number^");
        }
        double steps = (lhat_number_as_real(value) - kind->origin) / kind->step;
        // B2: the nearest step. Half a step past either end still rounds onto it.
        if (!(steps >= -0.5) || !(steps <= (double)kind->span + 0.5)) {
            return refuse(machine, place, "is out of range");
        }
        uint64_t packed = (uint64_t)llround(steps);
        put_bits(out, at, packed > kind->span ? kind->span : packed, kind->width);
        return true;
    }
    case KIND_ENUM: {
        const LhatEnumerator *member =
            lhat_is_object_kind(value, LHAT_OBJECT_ENUMERATOR)
                ? (const LhatEnumerator *)lhat_as_object(value)
                : NULL;
        if (member == NULL || member->owner != kind->owner) {
            return refuse(machine, place, "is not a member of the enum^");
        }
        put_bits(out, at, (uint64_t)(member->index - 1), kind->width);
        return true;
    }
    case KIND_ARRAY: {
        const LhatTable *items = as_table(value);
        if (items == NULL) {
            return refuse(machine, place, "is not a table");
        }
        for (size_t i = 0; i < kind->length; i++) {
            Place inside = {place.field, (int64_t)i};
            LhatValue item = lhat_table_get(items, lhat_integer((int64_t)i));
            if (lhat_is_nil(item)) {
                return refuse(machine, inside, "is missing");
            }
            if (!encode_kind(machine, kind->element, item, out, at, inside)) {
                return false;
            }
        }
        return true;
    }
    case KIND_FORMAT: {
        const LhatTable *table = as_table(value);
        if (table == NULL) {
            return refuse(machine, place, "is not a table");
        }
        for (size_t i = 0; i < kind->field_count; i++) {
            const Field *field = &kind->fields[i];
            Place inside = {field, -1};
            LhatValue item = lhat_table_get_bytes(table, field->name, field->length);
            if (lhat_is_nil(item)) {
                return refuse(machine, inside, "is missing");
            }
            if (!encode_kind(machine, field->kind, item, out, at, inside)) {
                return false;
            }
        }
        return true;
    }
    }
    return false;
}

// Packs `value` into `out`, which has room for the format's bytes. False
// once a panic has been raised.
static bool encode_into(LhatMachine *machine, const Kind *format, LhatValue value,
                        uint8_t *out)
{
    size_t size = (size_t)((format->bits + 7) / 8);
    memset(out, 0, size);
    uint64_t at = 0;
    Place place = {NULL, -1};
    return encode_kind(machine, format, value, out, &at, place);
}

static void format_encode(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count,
                          LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    const Kind *format = (const Kind *)live(arguments[0], shared.format_tag);
    if (format == NULL) {
        lhat_machine_panic_text(machine, "std.binary: the format was disposed");
        return;
    }
    size_t size = (size_t)((format->bits + 7) / 8);
    uint8_t small[256];
    uint8_t *out = size <= sizeof small ? small : (uint8_t *)lhat_alloc(size);
    if (out == NULL) {
        out_of_memory(machine);
        return;
    }
    LhatValue text = lhat_nil();
    if (encode_into(machine, format, arguments[1], out)) {
        if (lhat_machine_make_string(machine, (const char *)out, size, &text)) {
            answers[0] = text;
            *answer_count = 1;
        } else {
            out_of_memory(machine);
        }
    }
    if (out != small) {
        lhat_free(out);
    }
}

static void format_encode_into(LhatMachine *machine, void *context,
                               const LhatValue *arguments, size_t count,
                               LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    (void)answers;
    (void)answer_count;
    const Kind *format = (const Kind *)live(arguments[0], shared.format_tag);
    LhatBinaryBytes *bytes = bytes_of(arguments[2]);
    if (format == NULL || bytes == NULL) {
        lhat_machine_panic_text(machine, "std.binary: the format or the bytes were disposed");
        return;
    }
    if (!bytes_resize(bytes, (size_t)((format->bits + 7) / 8))) {
        out_of_memory(machine);
        return;
    }
    if (!encode_into(machine, format, arguments[1], bytes->data)) {
        bytes->length = 0;
    }
}

// ---------------------------------------------------------------------------
// Decoding

typedef enum { DECODED, MALFORMED, NO_MEMORY } Decoded;

// The key `table` already holds for `field`, so that writing it again makes
// no new string; nil^ when the table has none of its own.
static LhatValue own_key(const LhatTable *table, const Field *field)
{
    const LhatTable *found = NULL;
    uint32_t at = 0;
    bool inherited = false;
    LhatValue through = lhat_nil();
    lhat_table_locate_bytes(table, field->name, field->length, &found, &at,
                            &inherited, &through);
    return found == table && !inherited ? table->entries[at].key : lhat_nil();
}

// Reads one value of `kind` into `*out`. `reuse` is what stood in its place
// before: a table there is filled rather than replaced.
static Decoded decode_kind(LhatMachine *machine, const Kind *kind, const uint8_t *in,
                           uint64_t *at, LhatValue reuse, LhatValue *out)
{
    switch (kind->tag) {
    case KIND_UINT:
    case KIND_RANGE: {
        uint64_t packed = get_bits(in, at, kind->width);
        if (packed > kind->span || (kind->tag == KIND_UINT && packed > (uint64_t)INT64_MAX)) {
            return MALFORMED;
        }
        *out = lhat_integer((int64_t)((uint64_t)kind->low + packed));
        return DECODED;
    }
    case KIND_INT: {
        uint64_t packed = get_bits(in, at, kind->width);
        if (kind->width < 64 && (packed >> (kind->width - 1)) != 0) {
            packed |= UINT64_MAX << kind->width;
        }
        *out = lhat_integer((int64_t)packed);
        return DECODED;
    }
    case KIND_BOOL:
        *out = lhat_bool(get_bits(in, at, 1) != 0);
        return DECODED;
    case KIND_FIXED: {
        uint64_t packed = get_bits(in, at, kind->width);
        if (packed > kind->span) {
            return MALFORMED;
        }
        *out = lhat_real(kind->origin + (double)packed * kind->step);
        return DECODED;
    }
    case KIND_ENUM: {
        uint64_t packed = get_bits(in, at, kind->width);
        if (packed > kind->span || kind->members[packed] == NULL) {
            return MALFORMED;
        }
        *out = lhat_object((LhatObject *)kind->members[packed]);
        return DECODED;
    }
    case KIND_ARRAY:
    case KIND_FORMAT: {
        LhatTable *table = as_table(reuse);
        if (table == NULL) {
            LhatValue made = lhat_nil();
            if (!lhat_machine_make_table(machine, &made)) {
                return NO_MEMORY;
            }
            reuse = made;
            table = as_table(made);
        }
        size_t parts = kind->tag == KIND_ARRAY ? kind->length : kind->field_count;
        for (size_t i = 0; i < parts; i++) {
            const Field *field = kind->tag == KIND_FORMAT ? &kind->fields[i] : NULL;
            LhatValue key = lhat_integer((int64_t)i);
            if (field != NULL) {
                key = own_key(table, field);
                if (lhat_is_nil(key) &&
                    !lhat_machine_make_string(machine, field->name, field->length, &key)) {
                    return NO_MEMORY;
                }
            }
            LhatValue item = lhat_nil();
            Decoded decoded = decode_kind(machine, field != NULL ? field->kind : kind->element,
                                          in, at, lhat_table_get(table, key), &item);
            bool refused = false;
            if (decoded != DECODED) {
                return decoded;
            }
            if (!lhat_machine_table_set(machine, table, key, item, &refused) || refused) {
                return NO_MEMORY;
            }
        }
        *out = reuse;
        return DECODED;
    }
    }
    return MALFORMED;
}

// The bytes a string^ or a Bytes holds. False for anything else, which
// includes a disposed Bytes.
static bool input_bytes(LhatValue value, const uint8_t **data, size_t *length)
{
    const LhatString *text = as_string(value);
    if (text != NULL) {
        *data = (const uint8_t *)text->text;
        *length = text->length;
        return true;
    }
    LhatBinaryBytes *bytes = bytes_of(value);
    if (bytes != NULL) {
        *data = bytes->data;
        *length = bytes->length;
        return true;
    }
    return false;
}

static bool fail(LhatMachine *machine, const LhatErrorKind *kind, const char *message,
                 LhatValue *answers, int *answer_count)
{
    LhatValue error = lhat_nil();
    if (!lhat_machine_make_error(machine, kind, message, lhat_nil(), &error)) {
        out_of_memory(machine);
        return false;
    }
    answers[0] = error;
    *answer_count = 1;
    return false;
}

static void decode(LhatMachine *machine, const LhatValue *arguments, LhatValue reuse,
                   bool answer_table, LhatValue *answers, int *answer_count)
{
    const Kind *format = (const Kind *)live(arguments[0], shared.format_tag);
    const uint8_t *data = NULL;
    size_t length = 0;
    if (format == NULL || !input_bytes(arguments[1], &data, &length)) {
        lhat_machine_panic_text(machine, "std.binary: the format or the bytes were disposed");
        return;
    }
    // 11 の 3.5: longer is not wrong; what follows the format is left alone.
    if ((uint64_t)length < (format->bits + 7) / 8) {
        fail(machine, shared.truncated, "shorter than the format", answers, answer_count);
        return;
    }
    uint64_t at = 0;
    LhatValue table = lhat_nil();
    switch (decode_kind(machine, format, data, &at, reuse, &table)) {
    case DECODED:
        if (answer_table) {
            answers[0] = table;
            *answer_count = 1;
        }
        return;
    case MALFORMED:
        fail(machine, shared.malformed, "a number outside its kind", answers, answer_count);
        return;
    case NO_MEMORY:
        out_of_memory(machine);
        return;
    }
}

static void format_decode(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count,
                          LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    decode(machine, arguments, lhat_nil(), true, answers, answer_count);
}

static void format_decode_into(LhatMachine *machine, void *context,
                               const LhatValue *arguments, size_t count,
                               LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    if (as_table(arguments[2]) == NULL) {
        lhat_machine_panic_text(machine, "std.binary.decodeInto: expected a table");
        return;
    }
    decode(machine, arguments, arguments[2], false, answers, answer_count);
}

// ---------------------------------------------------------------------------
// Bytes

static LhatBinaryBytes *bytes_of(LhatValue value)
{
    return shared.bytes_tag != NULL ? (LhatBinaryBytes *)live(value, shared.bytes_tag) : NULL;
}

static bool bytes_resize(LhatBinaryBytes *bytes, size_t length)
{
    if (length > bytes->capacity) {
        uint8_t *grown = (uint8_t *)lhat_realloc(bytes->data, length);
        if (grown == NULL) {
            return false;
        }
        bytes->data = grown;
        bytes->capacity = length;
    }
    bytes->length = length;
    return true;
}

static void binary_bytes(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count,
                         LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)arguments;
    (void)count;
    LhatBinaryBytes *bytes = (LhatBinaryBytes *)lhat_calloc(1, sizeof *bytes);
    LhatValue out = lhat_nil();
    if (bytes == NULL || !lhat_machine_make_hostdata(machine, shared.bytes_tag, bytes, &out)) {
        lhat_free(bytes);
        out_of_memory(machine);
        return;
    }
    answers[0] = out;
    *answer_count = 1;
}

static void bytes_size(LhatMachine *machine, void *context,
                       const LhatValue *arguments, size_t count,
                       LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    LhatBinaryBytes *bytes = bytes_of(arguments[0]);
    if (bytes == NULL) {
        lhat_machine_panic_text(machine, "std.binary: the bytes were disposed");
        return;
    }
    answers[0] = lhat_integer((int64_t)bytes->length);
    *answer_count = 1;
}

static void bytes_to_string(LhatMachine *machine, void *context,
                            const LhatValue *arguments, size_t count,
                            LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    LhatBinaryBytes *bytes = bytes_of(arguments[0]);
    LhatValue text = lhat_nil();
    if (bytes == NULL) {
        lhat_machine_panic_text(machine, "std.binary: the bytes were disposed");
        return;
    }
    if (!lhat_machine_make_string(machine, (const char *)bytes->data, bytes->length, &text)) {
        out_of_memory(machine);
        return;
    }
    answers[0] = text;
    *answer_count = 1;
}

static void bytes_dispose(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count,
                          LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    (void)count;
    (void)answers;
    (void)answer_count;
    LhatBinaryBytes *bytes =
        (LhatBinaryBytes *)lhat_hostdata_pointer(arguments[0], shared.bytes_tag);
    if (bytes != NULL) {
        lhat_free(bytes->data);
        lhat_free(bytes);
    }
}

// ---------------------------------------------------------------------------

#define M "std.binary"

// Every registration's context, so that lhat_lookup_host_context finds it
// under any of them; binary.h names "bytes".
static const LhatBinaryInterface binary_interface = {LHAT_BINARY_INTERFACE_VERSION,
                                                     bytes_of, bytes_resize};

bool lhatstdlib_binary_register(LhatProgram *program)
{
    if (lhat_lookup_host_context(program, M, NULL, "bytes") != NULL) {
        return true;
    }
    static const char *const variants[] = {"Truncated", "Malformed"};
    const LhatErrorKind *found[2] = {NULL, NULL};
    if (!lhat_register_error_kind(program, M, "Error", variants, 2, NULL, found)) {
        return false;
    }
    shared.truncated = found[0];
    shared.malformed = found[1];
    shared.kind_tag = lhat_register_hostdata_type(program, M, "Kind");
    shared.format_tag =
        shared.kind_tag != NULL ? lhat_register_hostdata_subtype(program, M, "Format", M, "Kind")
                                : NULL;
    shared.bytes_tag = lhat_register_hostdata_type(program, M, "Bytes");
    if (shared.format_tag == NULL || shared.bytes_tag == NULL) {
        return false;
    }
    void *module = (void *)&binary_interface;
    return lhat_register_func(program, M, "uint", "f^number^ -> std.binary.Kind;", binary_uint, module) &&
           lhat_register_func(program, M, "int", "f^number^ -> std.binary.Kind;", binary_int, module) &&
           lhat_register_func(program, M, "bool", "f^ -> std.binary.Kind;", binary_bool, module) &&
           lhat_register_func(program, M, "range", "f^number^, number^ -> std.binary.Kind;",
                              binary_range, module) &&
           lhat_register_func(program, M, "fixed", "f^number^, number^, number^ -> std.binary.Kind;",
                              binary_fixed, module) &&
           lhat_register_func(program, M, "enum", "f^any^ -> std.binary.Kind;", binary_enum, module) &&
           lhat_register_func(program, M, "array", "f^number^, std.binary.Kind -> std.binary.Kind;",
                              binary_array, module) &&
           lhat_register_func(program, M, "format", "f^t^{} -> std.binary.Format;", binary_format,
                              module) &&
           lhat_register_func(program, M, "bytes", "f^ -> std.binary.Bytes;", binary_bytes, module) &&
           lhat_register_member(program, M, "Kind", "bits", "f^self^ -> number^;", kind_bits, module) &&
           lhat_register_member(program, M, "Kind", "dispose", "p^self^;", kind_dispose, module) &&
           lhat_register_member(program, M, "Format", "size", "f^self^ -> number^;", format_size,
                                module) &&
           lhat_register_member(program, M, "Format", "encode", "f^self^, t^{} -> string^;",
                                format_encode, module) &&
           lhat_register_member(program, M, "Format", "encodeInto",
                                "p^self^, t^{}, std.binary.Bytes;", format_encode_into, module) &&
           lhat_register_member(program, M, "Format", "decode",
                                "f^self^, string^|std.binary.Bytes -> t^{}|std.binary.Error;",
                                format_decode, module) &&
           lhat_register_member(program, M, "Format", "decodeInto",
                                "p^self^, string^|std.binary.Bytes, t^{} -> nil^|std.binary.Error;",
                                format_decode_into, module) &&
           lhat_register_member(program, M, "Bytes", "size", "f^self^ -> number^;", bytes_size, module) &&
           lhat_register_member(program, M, "Bytes", "toString", "f^self^ -> string^;",
                                bytes_to_string, module) &&
           lhat_register_member(program, M, "Bytes", "dispose", "p^self^;", bytes_dispose, module);
}
