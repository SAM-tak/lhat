// Private ABI: all L^ layouts, inline functions and ownership stay in C.
#include "lhat.h"
#include <stdio.h>
#include <string.h>

typedef struct { LhatValue value; } RustValue;
typedef struct {
    int kind; // nil, bool, integer, real, bytes; all other kinds are read-only
    int64_t integer;
    double real;
    const unsigned char *bytes;
    size_t length;
} RustData;
typedef struct {
    LhatMachine *machine;
    const LhatValue *arguments;
    size_t count;
    LhatValue *answers;
    int *answer_count;
} RustCall;
typedef void (*RustCallback)(void *, RustCall *);
typedef struct { RustCallback call; void *context; } RustHost;

static RustData read_data(LhatValue v)
{
    RustData d = {0};
    if (lhat_is_bool(v)) { d.kind = 1; d.integer = lhat_as_bool(v); }
    else if (lhat_is_integer(v)) { d.kind = 2; d.integer = lhat_as_integer(v); }
    else if (lhat_is_real(v)) { d.kind = 3; d.real = lhat_as_real(v); }
    else if (lhat_is_object_kind(v, LHAT_OBJECT_STRING)) {
        const LhatString *s = (const LhatString *)lhat_as_object(v);
        d.kind = 4; d.bytes = (const unsigned char *)s->text; d.length = s->length;
    } else if (!lhat_is_nil(v)) {
        d.kind = 5;
        if (lhat_is_object_kind(v, LHAT_OBJECT_TABLE)) d.kind = 6;
        if (lhat_is_object_kind(v, LHAT_OBJECT_COROUTINE)) d.kind = 7;
    }
    return d;
}

static bool make_data(LhatMachine *m, RustData d, LhatValue *out)
{
    switch (d.kind) {
    case 0: *out = lhat_nil(); return true;
    case 1: *out = lhat_bool(d.integer != 0); return true;
    case 2: *out = lhat_integer(d.integer); return true;
    case 3: *out = lhat_real(d.real); return true;
    case 4: return lhat_machine_make_string(m, (const char *)d.bytes, d.length, out);
    default: return false;
    }
}

static RustValue *hold(LhatMachine *m, LhatValue v)
{
    // Host-value runs point into temporary VM scratch, so never retain them.
    if (lhat_is_hostvalue(v) || lhat_is_run(v) || v.tag == LHAT_VALUE_CONT) return NULL;
    RustValue *h = lhat_alloc(sizeof(*h));
    if (!h) return NULL;
    h->value = v;
    bool refused = false;
    if (!lhat_machine_table_set(m, lhat_machine_host_root(m),
            lhat_integer((int64_t)(intptr_t)h), v, &refused) || refused) {
        lhat_free(h); return NULL;
    }
    return h;
}

void lhat_rs_value_free(LhatMachine *m, RustValue *v)
{
    bool refused = false;
    lhat_machine_table_set(m, lhat_machine_host_root(m),
        lhat_integer((int64_t)(intptr_t)v), lhat_nil(), &refused);
    lhat_free(v);
}
RustData lhat_rs_value_data(const RustValue *v) { return read_data(v->value); }
RustValue *lhat_rs_value_new(LhatMachine *m, RustData d)
{
    LhatValue v;
    return make_data(m, d, &v) ? hold(m, v) : NULL;
}
RustValue *lhat_rs_table_new(LhatMachine *m)
{
    LhatValue v;
    return lhat_machine_make_table(m, &v) ? hold(m, v) : NULL;
}
RustValue *lhat_rs_table_get(LhatMachine *m, const RustValue *t, const RustValue *k)
{
    if (!lhat_is_object_kind(t->value, LHAT_OBJECT_TABLE)) return NULL;
    return hold(m, lhat_table_get((LhatTable *)lhat_as_object(t->value), k->value));
}
bool lhat_rs_table_set(LhatMachine *m, const RustValue *t, const RustValue *k, const RustValue *v)
{
    if (!lhat_is_object_kind(t->value, LHAT_OBJECT_TABLE)) return false;
    bool refused = false;
    return lhat_machine_table_set(m, (LhatTable *)lhat_as_object(t->value),
                                  k->value, v->value, &refused) && !refused;
}
size_t lhat_rs_value_text(const RustValue *v, char *out, size_t capacity)
{ return lhat_value_text(v->value, out, capacity); }
bool lhat_rs_value_equal(const RustValue *a, const RustValue *b)
{ return lhat_value_equal(a->value, b->value); }
bool lhat_rs_coroutine_done(const RustValue *v)
{ return lhat_machine_coroutine_done(v->value); }

static void host_call(LhatMachine *m, void *context, const LhatValue *args,
                      size_t count, LhatValue *answers, int *answer_count)
{
    RustHost *host = context;
    RustCall call = {m, args, count, answers, answer_count};
    host->call(host->context, &call);
}
static void host_free(void *context) { lhat_free(context); }
bool lhat_rs_register(LhatProgram *p, const char *module, const char *name,
                      const char *signature, RustCallback call, void *context)
{
    RustHost *h = lhat_alloc(sizeof(*h));
    if (!h) return false;
    h->call = call; h->context = context;
    // Once installed, even a rejected registration may retain its context.
    if (!lhat_program_on_dispose(p, host_free, h)) { lhat_free(h); return false; }
    return lhat_register_func(p, module, name, signature, host_call, h);
}
bool lhat_rs_register_constant(LhatProgram *p, const char *module,
                               const char *name, RustData d)
{
    switch (d.kind) {
    case 1: return lhat_register_const_bool(p, module, NULL, name, d.integer != 0);
    case 2: return lhat_register_const_integer(p, module, NULL, name, d.integer);
    case 3: return lhat_register_const_real(p, module, NULL, name, d.real);
    case 4: {
        if (d.length == SIZE_MAX || memchr(d.bytes, 0, d.length)) return false;
        char *s = lhat_alloc(d.length + 1);
        if (!s) return false;
        memcpy(s, d.bytes, d.length); s[d.length] = 0;
        bool ok = lhat_register_const_string(p, module, NULL, name, s);
        lhat_free(s); return ok;
    }
    default: return false;
    }
}
size_t lhat_rs_program_diagnostic(LhatProgram *p, size_t i, char *out, size_t cap)
{
    const LhatProgramDiagnostic *d = lhat_program_diagnostic(p, i);
    if (!d) return 0;
    int n = snprintf(out, cap, "%s: %s", d->path ? d->path : "",
                     lhat_program_error_message(d->code));
    return n > 0 ? (size_t)n : 0;
}
size_t lhat_rs_arg_count(const RustCall *c) { return c->count; }
bool lhat_rs_arg(const RustCall *c, size_t i, RustData *out)
{
    if (i >= c->count) return false;
    *out = read_data(c->arguments[i]); return true;
}
bool lhat_rs_answer(RustCall *c, RustData d)
{
    if (!c->answers || !c->answer_count || *c->answer_count >= LHAT_MAX_TUPLE) return false;
    LhatValue v;
    if (!make_data(c->machine, d, &v)) return false;
    c->answers[(*c->answer_count)++] = v; return true;
}
bool lhat_rs_answer_arg(RustCall *c, size_t i)
{
    if (i >= c->count || !c->answers || !c->answer_count ||
        *c->answer_count >= LHAT_MAX_TUPLE || lhat_is_hostvalue(c->arguments[i])) return false;
    c->answers[(*c->answer_count)++] = c->arguments[i]; return true;
}
void lhat_rs_callback_error(RustCall *c, const char *text)
{ lhat_machine_panic_text(c->machine, text); }

RustValue *lhat_rs_load(LhatProgram *p, LhatMachine *m, const char *name,
                       const char *bytes, size_t length)
{
    LhatProto *proto = NULL;
    if (lhat_program_load_text(p, name, bytes, length, &proto) != LHAT_LOAD_OK) return NULL;
    LhatValue closure;
    if (!lhat_machine_adopt_script(m, proto, &closure)) {
        // A failed closure allocation may already have transferred ownership.
        if (!lhat_proto_is_owned(proto)) lhat_proto_free(proto);
        return NULL;
    }
    return hold(m, closure);
}
RustValue *lhat_rs_check(LhatProgram *p, LhatMachine *m, const char *path)
{
    const LhatUnit *u = lhat_program_check(p, path);
    if (!u || !lhat_unit_ok(u) || !lhat_program_compile(p)) return NULL;
    LhatValue closure;
    return lhat_machine_make_closure(m, lhat_unit_proto(u), &closure) ? hold(m, closure) : NULL;
}
LhatRunResult *lhat_rs_run(LhatMachine *m, int mode, const RustValue *callee,
                          const RustValue *const *args, size_t count,
                          const char *member, size_t member_length)
{
    if (count > SIZE_MAX / sizeof(LhatValue)) return NULL;
    LhatRunResult *r = lhat_alloc(sizeof(*r));
    if (!r) return NULL;
    LhatValue *values = count ? lhat_alloc(count * sizeof(*values)) : NULL;
    if (count && !values) { lhat_free(r); return NULL; }
    for (size_t i = 0; i < count; ++i) values[i] = args[i]->value;
    switch (mode) {
    case 0: *r = lhat_machine_call(m, callee->value, values, count); break;
    case 1: *r = lhat_machine_resume(m, callee->value, values, count); break;
    case 2: *r = lhat_machine_continue(m); break;
    default: *r = lhat_machine_call_member(m, callee->value, member, member_length, values, count); break;
    }
    lhat_free(values); return r;
}
int lhat_rs_result_status(const LhatRunResult *r) { return r->status; }
bool lhat_rs_result_suspended(const LhatRunResult *r) { return r->status == LHAT_RUN_SUSPENDED; }
size_t lhat_rs_result_count(const LhatRunResult *r) { return r->position_count ? r->position_count : 1; }
RustValue *lhat_rs_result_value(LhatMachine *m, const LhatRunResult *r, size_t i)
{ return hold(m, r->position_count ? r->positions[i] : r->value); }
size_t lhat_rs_result_error(LhatMachine *m, const LhatRunResult *r, char *out, size_t cap)
{ return lhat_run_error_write(m, *r, NULL, out, cap); }
