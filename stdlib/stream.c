#include "stream.h"
#include "binary.h"
#include "sys.h"
#include <string.h>

typedef struct {
    const LhatHostDataTag *reader, *writer;
    const LhatBinaryInterface *binary;
    const LhatErrorKind *disposed, *out_of_memory;
} StreamModule;
// Hostdata tags and their methods are process identities, as in std.binary.
static StreamModule shared;
typedef struct { uint8_t *data; size_t length, capacity, position; } MemoryStream;

static LhatValue stream_error(LhatMachine *machine, const LhatErrorKind *kind, const char *message)
{
    LhatValue out = lhat_nil();
    if (!lhat_machine_make_error(machine, kind, message, lhat_nil(), &out))
        lhat_machine_panic_text(machine, "out of memory");
    return out;
}

static MemoryStream *live(LhatValue value, const LhatHostDataTag *tag)
{
    void *pointer = lhat_hostdata_pointer(value, tag);
    return pointer != NULL && !((const LhatHostData *)lhat_as_object(value))->released ? pointer : NULL;
}

static void memory_dispose(LhatMachine *machine, void *context, const LhatValue *args,
                            size_t count, LhatValue *answers, int *answer_count)
{
    (void)machine; (void)count; (void)answers; (void)answer_count;
    StreamModule *m = context;
    MemoryStream *s = lhat_hostdata_pointer(args[0], m->reader);
    if (s == NULL) s = lhat_hostdata_pointer(args[0], m->writer);
    if (s != NULL) { lhat_free(s->data); lhat_free(s); }
}

static void make_stream(LhatMachine *machine, void *context, const LhatValue *args,
                         LhatValue *answers, int *answer_count, bool reading)
{
    StreamModule *m = context;
    MemoryStream *s = lhat_calloc(1, sizeof *s);
    if (s != NULL && reading) {
        const uint8_t *data = NULL; size_t n = 0;
        if (lhat_is_object_kind(args[0], LHAT_OBJECT_STRING)) {
            const LhatString *text = (const LhatString *)lhat_as_object(args[0]);
            data = (const uint8_t *)text->text; n = text->length;
        } else {
            LhatBinaryBytes *bytes = m->binary->bytes(args[0]);
            if (bytes == NULL) {
                lhat_free(s);
                answers[0] = stream_error(machine, m->disposed, "the source Bytes were disposed");
                *answer_count = 1; return;
            }
            data = bytes->data; n = bytes->length;
        }
        s->data = n ? lhat_alloc(n) : NULL;
        if (n && s->data == NULL) { lhat_free(s); s = NULL; }
        else { if (n) memcpy(s->data, data, n); s->length = s->capacity = n; }
    }
    if (s == NULL || !lhat_machine_make_hostdata(machine, reading ? m->reader : m->writer, s, answers)) {
        if (s != NULL) { lhat_free(s->data); lhat_free(s); }
        answers[0] = stream_error(machine, m->out_of_memory, "out of memory");
    }
    *answer_count = 1;
}
static void reader(LhatMachine *m, void *c, const LhatValue *a, size_t n, LhatValue *o, int *k)
{ (void)n; make_stream(m, c, a, o, k, true); }
static void writer(LhatMachine *m, void *c, const LhatValue *a, size_t n, LhatValue *o, int *k)
{ (void)n; make_stream(m, c, a, o, k, false); }

static void memory_read(LhatMachine *machine, void *context, const LhatValue *args,
                         size_t count, LhatValue *answers, int *answer_count)
{
    (void)count; StreamModule *m = context;
    MemoryStream *s = live(args[0], m->reader);
    LhatBinaryBytes *buffer = m->binary->bytes(args[1]);
    if (s == NULL || buffer == NULL) {
        answers[0] = stream_error(machine, m->disposed, "reader or buffer was disposed");
    } else {
        // Incoming length is the maximum request; outgoing length is bytes read.
        size_t n = buffer->length < s->length - s->position ? buffer->length : s->length - s->position;
        if (n) memcpy(buffer->data, s->data + s->position, n);
        buffer->length = n; s->position += n;
        answers[0] = lhat_integer((LhatInteger)n);
    }
    *answer_count = 1;
}

static void memory_write(LhatMachine *machine, void *context, const LhatValue *args,
                          size_t count, LhatValue *answers, int *answer_count)
{
    (void)count; StreamModule *m = context;
    MemoryStream *s = live(args[0], m->writer);
    LhatBinaryBytes *buffer = m->binary->bytes(args[1]);
    answers[0] = lhat_nil(); *answer_count = 1;
    if (s == NULL || buffer == NULL) {
        answers[0] = stream_error(machine, m->disposed, "writer or buffer was disposed"); return;
    }
    if (buffer->length > SIZE_MAX - s->length) goto oom;
    size_t need = s->length + buffer->length;
    if (need > s->capacity) {
        size_t room = s->capacity < 256 ? 256 : s->capacity;
        while (room < need) { if (room > SIZE_MAX / 2) { room = need; break; } room *= 2; }
        uint8_t *grown = lhat_realloc(s->data, room);
        if (grown == NULL) goto oom;
        s->data = grown; s->capacity = room;
    }
    if (buffer->length) memcpy(s->data + s->length, buffer->data, buffer->length);
    s->length = need; return;
oom:
    answers[0] = stream_error(machine, m->out_of_memory, "out of memory");
}

static void memory_text(LhatMachine *machine, void *context, const LhatValue *args,
                         size_t count, LhatValue *answers, int *answer_count)
{
    (void)count; StreamModule *m = context;
    MemoryStream *s = live(args[0], m->writer);
    if (s == NULL) answers[0] = stream_error(machine, m->disposed, "writer was disposed");
    else if (!lhat_machine_make_string(machine, s->length ? (const char *)s->data : "", s->length, answers))
        answers[0] = stream_error(machine, m->out_of_memory, "out of memory");
    *answer_count = 1;
}

bool lhatstdlib_stream_register(LhatProgram *program)
{
    if (lhat_lookup_host_context(program, "std.stream", NULL, "writer") != NULL) return true;
    if (!lhatstdlib_binary_register(program) || !lhatstdlib_sys_register(program)) return false;
    StreamModule *m = &shared;
    m->binary = lhat_lookup_host_context(program, "std.binary", NULL, "bytes");
    m->out_of_memory = lhatstdlib_sys_lookup(program, "OutOfMemory");
    const char *variants[] = {"Disposed"};
    if (!lhat_register_error_kind(program, "std.stream", "Error", variants, 1, NULL, &m->disposed)) return false;
    m->reader = lhat_register_hostdata_type(program, "std.stream", "MemoryReader");
    m->writer = lhat_register_hostdata_type(program, "std.stream", "MemoryWriter");
    if (m->reader == NULL || m->writer == NULL) return false;
    return lhat_register_func(program, "std.stream", "reader",
        "f^string^|std.binary.Bytes -> std.stream.MemoryReader|std.stream.Error|std.sys.Error.OutOfMemory;", reader, m) &&
        lhat_register_func(program, "std.stream", "writer",
        "f^ -> std.stream.MemoryWriter|std.sys.Error.OutOfMemory;", writer, m) &&
        lhat_register_member(program, "std.stream", "MemoryReader", "read",
        "p^self^,std.binary.Bytes -> number^|std.stream.Error;", memory_read, m) &&
        lhat_register_member(program, "std.stream", "MemoryWriter", "write",
        "p^self^,std.binary.Bytes -> nil^|std.stream.Error|std.sys.Error.OutOfMemory;", memory_write, m) &&
        lhat_register_member(program, "std.stream", "MemoryWriter", "toString",
        "f^self^ -> string^|std.stream.Error|std.sys.Error.OutOfMemory;", memory_text, m) &&
        lhat_register_member(program, "std.stream", "MemoryReader", "dispose", "p^self^;", memory_dispose, m) &&
        lhat_register_member(program, "std.stream", "MemoryWriter", "dispose", "p^self^;", memory_dispose, m);
}
