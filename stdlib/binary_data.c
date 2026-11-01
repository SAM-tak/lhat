// Schema-free table records, independent of the compiler and bytecode format.
#include "binary.h"
#include "sys.h"
#include <math.h>
#include <string.h>

#define CHUNK 16384u
#define DEPTH 96u
static const uint8_t magic[8] = {0x89, 'L', 'H', 'T', 1, '\r', '\n', 0x1a};
enum { DATA_NIL, DATA_FALSE, DATA_TRUE, DATA_INT, DATA_REAL, DATA_STRING, DATA_TABLE };

typedef struct {
    LhatMachine *machine;
    LhatValue roots;
    LhatBinaryWriteFn write;
    LhatBinaryReadFn read;
    void *context;
    LhatBinaryDataLimits limits;
    LhatBinaryDataStatus status;
    size_t bytes, entries, used, position;
    uint8_t buffer[CHUNK];
    const LhatTable *ancestors[DEPTH];
} Codec;

static bool pin(Codec *c, size_t at, LhatValue value)
{
    bool refused = false;
    if (!lhat_machine_table_set(c->machine, (LhatTable *)lhat_as_object(c->roots),
                               lhat_integer((LhatInteger)at), value, &refused) || refused) {
        c->status = LHAT_BINARY_DATA_OUT_OF_MEMORY;
        return false;
    }
    return true;
}

static bool begin(Codec *c, LhatMachine *machine, const LhatBinaryDataLimits *limits)
{
    c->machine = machine;
    c->limits = limits != NULL ? *limits
        : (LhatBinaryDataLimits){256u * 1024u * 1024u, 1000000u, DEPTH};
    if (machine == NULL || c->limits.max_depth > DEPTH || c->limits.max_depth == 0) {
        c->status = LHAT_BINARY_DATA_LIMIT;
        return false;
    }
    bool refused = false;
    if (!lhat_machine_make_table(machine, &c->roots) ||
        !lhat_machine_table_set(machine, lhat_machine_host_root(machine),
                                c->roots, c->roots, &refused) || refused) {
        c->status = LHAT_BINARY_DATA_OUT_OF_MEMORY;
        return false;
    }
    return true;
}

static void end(Codec *c)
{
    bool refused = false;
    if (lhat_is_object_kind(c->roots, LHAT_OBJECT_TABLE)) {
        lhat_machine_table_set(c->machine, lhat_machine_host_root(c->machine),
                              c->roots, lhat_nil(), &refused);
    }
}

static bool budget(Codec *c, size_t n)
{
    if (c->bytes > c->limits.max_bytes || n > c->limits.max_bytes - c->bytes) {
        c->status = LHAT_BINARY_DATA_LIMIT;
        return false;
    }
    c->bytes += n;
    return true;
}

static bool raw_write(Codec *c, const uint8_t *data, size_t n)
{
    if (c->status != LHAT_BINARY_DATA_OK || !budget(c, n)) return false;
    if (!c->write(c->context, data, n)) {
        c->status = LHAT_BINARY_DATA_IO;
        return false;
    }
    return true;
}

static bool raw_read(Codec *c, uint8_t *data, size_t n)
{
    if (c->status != LHAT_BINARY_DATA_OK || !budget(c, n)) return false;
    while (n > 0) {
        size_t got = 0;
        if (!c->read(c->context, data, n, &got) || got > n) {
            c->status = LHAT_BINARY_DATA_IO;
            return false;
        }
        if (got == 0) { c->status = LHAT_BINARY_DATA_TRUNCATED; return false; }
        data += got;
        n -= got;
    }
    return true;
}

static bool flush_chunk(Codec *c)
{
    if (c->used == 0) return true;
    uint32_t n = (uint32_t)c->used;
    uint8_t header[4] = {(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)};
    if (!raw_write(c, header, 4) || !raw_write(c, c->buffer, c->used)) return false;
    c->used = 0;
    return true;
}

static bool put(Codec *c, const void *data, size_t n)
{
    const uint8_t *p = data;
    while (n > 0 && c->status == LHAT_BINARY_DATA_OK) {
        size_t room = CHUNK - c->used;
        size_t take = n < room ? n : room;
        memcpy(c->buffer + c->used, p, take);
        c->used += take; p += take; n -= take;
        if (c->used == CHUNK && !flush_chunk(c)) return false;
    }
    return c->status == LHAT_BINARY_DATA_OK;
}

static bool next_chunk(Codec *c)
{
    uint8_t h[4];
    if (!raw_read(c, h, 4)) return false;
    uint32_t n = (uint32_t)h[0] | (uint32_t)h[1] << 8 |
                 (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
    if (n == 0 || n > CHUNK) {
        c->status = n == 0 ? LHAT_BINARY_DATA_TRUNCATED : LHAT_BINARY_DATA_MALFORMED;
        return false;
    }
    c->position = 0;
    c->used = n;
    return raw_read(c, c->buffer, n);
}

static bool get(Codec *c, void *data, size_t n)
{
    uint8_t *p = data;
    while (n > 0) {
        if (c->position == c->used && !next_chunk(c)) return false;
        size_t left = c->used - c->position;
        size_t take = n < left ? n : left;
        memcpy(p, c->buffer + c->position, take);
        c->position += take; p += take; n -= take;
    }
    return true;
}

static bool put_var(Codec *c, uint64_t n)
{
    uint8_t b[10]; size_t used = 0;
    do { b[used++] = (uint8_t)((n & 127) | (n > 127 ? 128 : 0)); n >>= 7; } while (n);
    return put(c, b, used);
}

static bool get_var(Codec *c, uint64_t *out)
{
    uint64_t n = 0;
    for (unsigned i = 0; i < 10; i++) {
        uint8_t b;
        if (!get(c, &b, 1)) return false;
        if ((i == 9 && b > 1) || (i > 0 && b == 0)) break;
        n |= (uint64_t)(b & 127) << (7 * i);
        if (!(b & 128)) { *out = n; return true; }
    }
    c->status = LHAT_BINARY_DATA_MALFORMED;
    return false;
}

static bool count_entries(Codec *c, uint64_t a, uint64_t b)
{
    if (a > c->limits.max_entries || b > c->limits.max_entries - (size_t)a ||
        c->entries > c->limits.max_entries - (size_t)(a + b)) {
        c->status = LHAT_BINARY_DATA_LIMIT;
        return false;
    }
    c->entries += (size_t)(a + b);
    return true;
}

static bool scalar_key(LhatValue value)
{
    return lhat_is_bool(value) || lhat_is_integer(value) ||
           (lhat_is_number(value) && isfinite(lhat_number_as_real(value))) ||
           lhat_is_object_kind(value, LHAT_OBJECT_STRING);
}

static bool write_value(Codec *c, LhatValue value, size_t depth)
{
    if (depth > c->limits.max_depth) { c->status = LHAT_BINARY_DATA_LIMIT; return false; }
    if (!pin(c, depth * 2, value)) return false;
    uint8_t tag;
    if (lhat_is_nil(value)) tag = DATA_NIL;
    else if (lhat_is_bool(value)) tag = lhat_as_bool(value) ? DATA_TRUE : DATA_FALSE;
    else if (lhat_is_integer(value)) tag = DATA_INT;
    else if (lhat_is_number(value) && isfinite(lhat_number_as_real(value))) tag = DATA_REAL;
    else if (lhat_is_object_kind(value, LHAT_OBJECT_STRING)) tag = DATA_STRING;
    else if (lhat_is_object_kind(value, LHAT_OBJECT_TABLE)) tag = DATA_TABLE;
    else { c->status = LHAT_BINARY_DATA_UNSUPPORTED; return false; }
    if (!put(c, &tag, 1)) return false;
    if (tag == DATA_INT || tag == DATA_REAL) {
        uint64_t bits;
        if (tag == DATA_INT) { int64_t n = lhat_as_integer(value); memcpy(&bits, &n, 8); }
        else { double n = lhat_number_as_real(value); memcpy(&bits, &n, 8); }
        uint8_t b[8];
        for (unsigned i = 0; i < 8; i++) b[i] = (uint8_t)(bits >> (8 * i));
        return put(c, b, 8);
    }
    if (tag == DATA_STRING) {
        const LhatString *s = (const LhatString *)lhat_as_object(value);
        return put_var(c, s->length) && put(c, s->text, s->length);
    }
    if (tag != DATA_TABLE) return true;
    const LhatTable *table = (const LhatTable *)lhat_as_object(value);
    if (depth >= c->limits.max_depth) { c->status = LHAT_BINARY_DATA_LIMIT; return false; }
    if (table->is_definition || table->definition != NULL) {
        c->status = LHAT_BINARY_DATA_UNSUPPORTED; return false;
    }
    for (size_t i = 0; i < depth; i++) {
        if (c->ancestors[i] == table) { c->status = LHAT_BINARY_DATA_CYCLE; return false; }
    }
    c->ancestors[depth] = table;
    size_t array = table->array_count, entries = table->entry_count;
    uint32_t version = table->version;
    if (!count_entries(c, array, entries) || !put_var(c, array) || !put_var(c, entries)) return false;
    for (size_t i = 0; i < array; i++) {
        if (table->version != version || table->array_count != array) goto changed;
        if (!write_value(c, lhat_slots_get(table->array, i), depth + 1)) return false;
    }
    size_t written = 0;
    for (size_t i = 0; i < table->entry_capacity; i++) {
        if (table->version != version || table->array_count != array) goto changed;
        LhatValue key = table->entries[i].key;
        if (lhat_is_nil(key)) continue;
        if (!scalar_key(key)) { c->status = LHAT_BINARY_DATA_UNSUPPORTED; return false; }
        // Keep the value alive even if a Writer callback changes this slot.
        LhatValue held = table->entries[i].value;
        if (!pin(c, depth * 2 + 1, held) || !write_value(c, key, depth + 1) ||
            !write_value(c, held, depth + 1)) return false;
        written++;
    }
    if (table->version != version || table->array_count != array || written != entries) goto changed;
    return true;
changed:
    c->status = LHAT_BINARY_DATA_MALFORMED;
    return false;
}

static bool read_value(Codec *c, LhatValue *out, size_t depth)
{
    *out = lhat_nil();
    if (depth > c->limits.max_depth) { c->status = LHAT_BINARY_DATA_LIMIT; return false; }
    uint8_t tag;
    if (!get(c, &tag, 1)) return false;
    if (tag == DATA_NIL) return true;
    if (tag == DATA_FALSE || tag == DATA_TRUE) { *out = lhat_bool(tag == DATA_TRUE); return true; }
    if (tag == DATA_INT || tag == DATA_REAL) {
        uint8_t b[8]; uint64_t bits = 0;
        if (!get(c, b, 8)) return false;
        for (unsigned i = 0; i < 8; i++) bits |= (uint64_t)b[i] << (8 * i);
        if (tag == DATA_INT) { int64_t n; memcpy(&n, &bits, 8); *out = lhat_integer(n); }
        else {
            double n; memcpy(&n, &bits, 8);
            if (!isfinite(n)) goto malformed;
            *out = lhat_real(n);
        }
        return true;
    }
    if (tag == DATA_STRING) {
        uint64_t n;
        if (!get_var(c, &n)) return false;
        if (n > SIZE_MAX || n > c->limits.max_bytes) {
            c->status = LHAT_BINARY_DATA_LIMIT; return false;
        }
        char *text = n ? lhat_alloc((size_t)n) : NULL;
        if (n && text == NULL) goto oom;
        bool ok = get(c, text, (size_t)n);
        if (ok) ok = lhat_machine_make_string(c->machine, text != NULL ? text : "", (size_t)n, out);
        lhat_free(text);
        if (!ok && c->status == LHAT_BINARY_DATA_OK) goto oom;
        return ok;
    }
    if (tag == DATA_TABLE) {
        if (depth >= c->limits.max_depth) { c->status = LHAT_BINARY_DATA_LIMIT; return false; }
        uint64_t array, entries;
        if (!get_var(c, &array) || !get_var(c, &entries) || !count_entries(c, array, entries)) return false;
        if (!lhat_machine_make_table(c->machine, out) || !pin(c, depth * 2, *out)) goto oom;
        LhatTable *table = (LhatTable *)lhat_as_object(*out);
        if (!lhat_table_resize(table, (size_t)array, lhat_nil())) goto oom;
        for (uint64_t i = 0; i < array; i++) {
            LhatValue value;
            if (!read_value(c, &value, depth + 1)) return false;
            bool refused = false;
            if (!lhat_machine_table_set(c->machine, table, lhat_integer((LhatInteger)i), value, &refused)) goto oom;
        }
        for (uint64_t i = 0; i < entries; i++) {
            LhatValue key, value;
            if (!read_value(c, &key, depth + 1)) return false;
            if (!scalar_key(key)) goto malformed;
            if (lhat_is_number(key)) {
                double n = lhat_number_as_real(key);
                if (n >= 0 && n <= (double)array && floor(n) == n) goto malformed;
            }
            if (!lhat_is_nil(lhat_table_get(table, key))) goto malformed;
            if (!pin(c, depth * 2 + 1, key) || !read_value(c, &value, depth + 1)) return false;
            if (lhat_is_nil(value)) goto malformed;
            bool refused = false;
            if (!lhat_machine_table_set(c->machine, table, key, value, &refused)) goto oom;
            if (refused) goto malformed;
        }
        return true;
    }
malformed:
    c->status = LHAT_BINARY_DATA_MALFORMED; return false;
oom:
    c->status = LHAT_BINARY_DATA_OUT_OF_MEMORY; return false;
}

LhatBinaryDataStatus lhatstdlib_binary_serialize(
    LhatMachine *machine, LhatValue table, LhatBinaryWriteFn write, void *context,
    const LhatBinaryDataLimits *limits)
{
    Codec c = {0}; c.write = write; c.context = context;
    if (write == NULL || !lhat_is_object_kind(table, LHAT_OBJECT_TABLE)) return LHAT_BINARY_DATA_UNSUPPORTED;
    if (begin(&c, machine, limits) && pin(&c, 0, table) && raw_write(&c, magic, sizeof magic) &&
        write_value(&c, table, 0) && flush_chunk(&c)) {
        const uint8_t terminal[4] = {0};
        raw_write(&c, terminal, 4);
    }
    end(&c);
    return c.status;
}

LhatBinaryDataStatus lhatstdlib_binary_deserialize(
    LhatMachine *machine, LhatBinaryReadFn read, void *context,
    const LhatBinaryDataLimits *limits, LhatValue *out)
{
    if (out == NULL || read == NULL) return LHAT_BINARY_DATA_MALFORMED;
    *out = lhat_nil();
    Codec c = {0}; c.read = read; c.context = context;
    uint8_t header[8];
    if (begin(&c, machine, limits) && raw_read(&c, header, 8)) {
        if (memcmp(header, magic, 8) != 0) c.status = LHAT_BINARY_DATA_MALFORMED;
        else if (read_value(&c, out, 0)) {
            uint8_t terminal[4];
            if (!lhat_is_object_kind(*out, LHAT_OBJECT_TABLE) || c.position != c.used)
                c.status = LHAT_BINARY_DATA_MALFORMED;
            else if (raw_read(&c, terminal, 4) &&
                     (terminal[0] | terminal[1] | terminal[2] | terminal[3]) != 0)
                c.status = LHAT_BINARY_DATA_MALFORMED;
        }
    }
    end(&c);
    if (c.status != LHAT_BINARY_DATA_OK) *out = lhat_nil();
    return c.status;
}

typedef struct { uint8_t *data; size_t length, capacity, position; } Memory;
static bool memory_write(void *context, const uint8_t *data, size_t n)
{
    Memory *m = context;
    if (n > SIZE_MAX - m->length) return false;
    size_t need = m->length + n;
    if (need > m->capacity) {
        size_t room = m->capacity < 256 ? 256 : m->capacity;
        while (room < need) { if (room > SIZE_MAX / 2) { room = need; break; } room *= 2; }
        uint8_t *grown = lhat_realloc(m->data, room);
        if (grown == NULL) return false;
        m->data = grown; m->capacity = room;
    }
    if (n) memcpy(m->data + m->length, data, n);
    m->length = need;
    return true;
}
static bool memory_read(void *context, uint8_t *data, size_t n, size_t *got)
{
    Memory *m = context;
    *got = n < m->length - m->position ? n : m->length - m->position;
    if (*got) memcpy(data, m->data + m->position, *got);
    m->position += *got;
    return true;
}

typedef struct {
    const LhatBinaryInterface *binary;
    const LhatErrorKind *errors[7];
} DataModule;

static LhatValue data_error(LhatMachine *machine, DataModule *module, LhatBinaryDataStatus status)
{
    static const char *const messages[] = {"", "truncated binary table record", "malformed binary table record or changed source",
        "unsupported binary table value", "cyclic table", "binary table resource limit exceeded", "stream operation failed", "out of memory"};
    LhatValue error = lhat_nil();
    if (!lhat_machine_make_error(machine, module->errors[status - 1], messages[status], lhat_nil(), &error))
        lhat_machine_panic_text(machine, "out of memory");
    return error;
}

static bool source_bytes(DataModule *module, LhatValue value, Memory *memory)
{
    if (lhat_is_object_kind(value, LHAT_OBJECT_STRING)) {
        const LhatString *text = (const LhatString *)lhat_as_object(value);
        memory->data = (uint8_t *)(void *)text->text; memory->length = text->length; return true;
    }
    LhatBinaryBytes *bytes = module->binary->bytes(value);
    if (bytes == NULL) return false;
    memory->data = bytes->data; memory->length = bytes->length; return true;
}

static void encode(LhatMachine *machine, void *context, const LhatValue *args, size_t count,
                    LhatValue *answers, int *answer_count)
{
    (void)count; DataModule *module = context; Memory memory = {0};
    LhatBinaryDataStatus status = lhatstdlib_binary_serialize(machine, args[0], memory_write, &memory, NULL);
    if (status == LHAT_BINARY_DATA_OK && !lhat_machine_make_string(machine, (const char *)memory.data, memory.length, answers))
        status = LHAT_BINARY_DATA_OUT_OF_MEMORY;
    if (status == LHAT_BINARY_DATA_IO) status = LHAT_BINARY_DATA_OUT_OF_MEMORY;
    if (status != LHAT_BINARY_DATA_OK) answers[0] = data_error(machine, module, status);
    lhat_free(memory.data); *answer_count = 1;
}

static void decode(LhatMachine *machine, void *context, const LhatValue *args, size_t count,
                    LhatValue *answers, int *answer_count)
{
    (void)count; DataModule *module = context; Memory memory = {0};
    LhatBinaryDataStatus status = source_bytes(module, args[0], &memory)
        ? lhatstdlib_binary_deserialize(machine, memory_read, &memory, NULL, answers)
        : LHAT_BINARY_DATA_MALFORMED;
    if (status == LHAT_BINARY_DATA_OK && memory.position != memory.length) status = LHAT_BINARY_DATA_MALFORMED;
    if (status != LHAT_BINARY_DATA_OK) answers[0] = data_error(machine, module, status);
    *answer_count = 1;
}

typedef struct {
    LhatMachine *machine;
    DataModule *module;
    LhatValue stream, buffer, error;
} Bridge;

static bool call_stream(Bridge *b, const char *name, LhatRunResult *ran)
{
    *ran = lhat_machine_call_member(b->machine, b->stream, name, strlen(name), &b->buffer, 1);
    if (ran->status != LHAT_RUN_OK) {
        if (ran->status == LHAT_RUN_PANIC) lhat_machine_panic(b->machine, ran->value);
        else lhat_machine_panic_text(b->machine, "binary stream callback failed");
        return false;
    }
    if (lhat_is_object_kind(ran->value, LHAT_OBJECT_ERROR)) { b->error = ran->value; return false; }
    return true;
}

static bool bridge_write(void *context, const uint8_t *data, size_t n)
{
    Bridge *b = context;
    LhatBinaryBytes *bytes = b->module->binary->bytes(b->buffer);
    if (bytes == NULL || !b->module->binary->resize(bytes, n)) return false;
    if (n) memcpy(bytes->data, data, n);
    LhatRunResult ran;
    return call_stream(b, "write", &ran) && lhat_is_nil(ran.value);
}

static bool bridge_read(void *context, uint8_t *data, size_t capacity, size_t *length)
{
    Bridge *b = context;
    LhatBinaryBytes *bytes = b->module->binary->bytes(b->buffer);
    if (bytes == NULL || !b->module->binary->resize(bytes, capacity)) return false;
    LhatRunResult ran;
    if (!call_stream(b, "read", &ran) || !lhat_is_integer(ran.value) || lhat_as_integer(ran.value) < 0) return false;
    bytes = b->module->binary->bytes(b->buffer);
    uint64_t n = (uint64_t)lhat_as_integer(ran.value);
    if (bytes == NULL || n > capacity || n != bytes->length) return false;
    *length = (size_t)n;
    if (*length) memcpy(data, bytes->data, *length);
    return true;
}

static void stream_operation(LhatMachine *machine, void *context, const LhatValue *args,
                              LhatValue *answers, int *answer_count, bool writing)
{
    Bridge b = {0}; b.machine = machine; b.module = context; b.stream = args[0]; b.error = lhat_nil();
    Codec roots = {0};
    LhatBinaryDataStatus status = LHAT_BINARY_DATA_OUT_OF_MEMORY;
    LhatValue result = lhat_nil();
    if (begin(&roots, machine, NULL) && pin(&roots, 0, b.stream) &&
        lhatstdlib_binary_make_bytes(machine, CHUNK, &b.buffer) && pin(&roots, 1, b.buffer)) {
        status = writing ? lhatstdlib_binary_serialize(machine, args[1], bridge_write, &b, NULL)
                         : lhatstdlib_binary_deserialize(machine, bridge_read, &b, NULL, &result);
    }
    end(&roots);
    answers[0] = result;
    if (status != LHAT_BINARY_DATA_OK) answers[0] = !lhat_is_nil(b.error) ? b.error : data_error(machine, b.module, status);
    *answer_count = 1;
}

static void serialize(LhatMachine *m, void *c, const LhatValue *a, size_t n, LhatValue *o, int *k)
{ (void)n; stream_operation(m, c, a, o, k, true); }
static void deserialize(LhatMachine *m, void *c, const LhatValue *a, size_t n, LhatValue *o, int *k)
{ (void)n; stream_operation(m, c, a, o, k, false); }

bool lhatstdlib_binary_data_register(LhatProgram *program)
{
    if (!lhatstdlib_sys_register(program)) return false;
    DataModule *module = lhat_calloc(1, sizeof *module);
    if (module == NULL) return false;
    if (!lhat_program_on_dispose(program, lhat_free, module)) { lhat_free(module); return false; }
    module->binary = lhat_lookup_host_context(program, "std.binary", NULL, "bytes");
    const char *variants[] = {"Truncated", "Malformed", "Unsupported", "Cycle", "Limit", "IO"};
    if (!lhat_register_error_kind(program, "std.binary", "DataError", variants, 6, NULL, module->errors)) return false;
    module->errors[6] = lhatstdlib_sys_lookup(program, "OutOfMemory");
    return lhat_register_func(program, "std.binary", "encode",
        "f^t^{} -> string^|std.binary.DataError|std.sys.Error.OutOfMemory;", encode, module) &&
        lhat_register_func(program, "std.binary", "decode",
        "f^string^|std.binary.Bytes -> t^{}|std.binary.DataError|std.sys.Error.OutOfMemory;", decode, module) &&
        lhat_register_func(program, "std.binary", "serialize",
        "p^t^{write:p^self^,std.binary.Bytes -> nil^|error^;},t^{} -> nil^|error^;", serialize, module) &&
        lhat_register_func(program, "std.binary", "deserialize",
        "p^t^{read:p^self^,std.binary.Bytes -> number^|error^;} -> t^{}|error^;", deserialize, module);
}
