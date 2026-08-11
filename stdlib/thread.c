// L^ (lhat) -- sample standard library: std.thread.
//
// spawn(fn, ...) starts fn -- p^... -> (number^|bool^|string^|nil^); with no
// upvalues -- on a fresh LhatMachine of its own, on a new OS thread, and hands
// it whatever else was written. join() blocks for the result and dispose()
// (05 の 8.8) blocks the same way if the caller never joined -- see
// thread_dispose's comment for why letting an unjoined thread run past its
// ThreadHandle's disposal is not safe here.
//
// A Machine/its GC heap is one OS thread's alone (src/gc.c's "one set of
// roots rather than a thread apiece"), so a value cannot simply be handed to
// another machine -- it would still point into the first one's heap.
// Everything that crosses the boundary here is copied into a plain C
// representation first (ThreadValue) and only turned back into an LhatValue on
// the machine that is going to use it. The same copy runs both ways: the
// arguments on the way in, the answer on the way back.
//
// What that copy can carry is what the boundary allows, and it is the whole of
// why fn is written 'p^...' rather than with parameters of its own. A table, a
// closure or a hostdata value has no plain representation, and there is no
// host API to build a table's contents from C, so only nil^, bool^, number^
// and string^ cross -- 13.7's collector is any^ underneath, which is exactly
// the shape that takes those four without a written parameter having to name
// them. What crosses is checked as the call is made, not before it: an
// argument of any other type answers ThreadError.BadArgument. See the
// registration at the foot of this file for why that is a run-time answer and
// not a type. A closure's Proto, unlike its upvalues, is safe to share -- see
// lhat_proto_new's comment on why it is always born already black.
//
// spawn/join answer a ThreadHandle or a ThreadError, or std.error.
// OutOfMemory for the one failure this module shares with every other
// stdlib module (see error.h) -- read the result with try^/catch^, or
// narrow with isa^ against std.thread.ThreadHandle (05 の 8.8's hostdata
// path).

#include "error.h"
#include "thread.h"

#include "port/thread.h"

#include <stdint.h>
#include <string.h>

// What lhatstdlib_thread_register made, threaded through as every registration's
// `context` (05 の 8.7) rather than kept in file-scope statics -- a second
// LhatProgram registering this module gets its own kinds/tag instead of
// silently overwriting the first program's, which would hand its already-
// checked code a different (04 の 2.4: identity-by-declaration) kind object
// than the one its signatures were checked against.
//
// Allocated once per lhatstdlib_thread_register call and never freed -- program.h
// has no hook to run when its LhatProgram is disposed, so a host that
// registers this module many times over a process's life leaks one of these
// per registration. Fine for a program set up once and run, which is this
// sample's whole use case; stdlib/random.c's module-wide state makes the same
// trade for the same reason.
typedef struct {
    const LhatErrorKind *not_spawnable;
    const LhatErrorKind *bad_argument;
    const LhatErrorKind *spawn_failed;
    const LhatErrorKind *bad_result;
    const LhatErrorKind *already_joined;
    const LhatErrorKind *out_of_memory;  // std.error.OutOfMemory -- error.h
    const LhatHostDataTag *handle_tag;
} ThreadModule;

typedef enum {
    CARRIED_NIL,
    CARRIED_BOOL,
    CARRIED_INT,
    CARRIED_REAL,
    CARRIED_STRING
} CarriedKind;

// One value in the only form that belongs to neither machine.
typedef struct {
    CarriedKind kind;
    union {
        bool boolean;
        int64_t integer;
        double real;
        struct {
            char *bytes;
            size_t length;
        } text;
    } as;
} ThreadValue;

typedef struct {
    const LhatProto *proto;      // borrowed; lives as long as the program
    const LhatModule *modules;   // borrowed; same
    size_t module_count;
    // 13.7's collector, still in the form that crossed. Owned by this, and
    // given back by thread_main as soon as the new machine has its own copy.
    ThreadValue *arguments;
    size_t argument_count;
    struct ThreadHandle *handle;  // owned by spawn's caller, not by this
} ThreadStart;

typedef struct ThreadHandle {
    LhatThread os;
    // Written once by thread_main, read only after the join -- the
    // happens-before a join establishes is what makes this safe without a
    // lock of its own. `joined` itself is touched only by whichever L^ code
    // holds this handle (one OS thread, since a value never crosses
    // machines), so it needs no lock either.
    bool joined;
    LhatRunStatus status;
    ThreadValue result;
} ThreadHandle;

static LhatValue fail_with(LhatMachine *machine, const LhatErrorKind *kind,
                           const char *message)
{
    LhatValue error = lhat_nil();
    return lhat_machine_make_error(machine, kind, message, lhat_nil(), &error)
               ? error
               : lhat_nil();
}

static bool to_thread_value(LhatValue value, ThreadValue *out)
{
    if (lhat_is_nil(value)) {
        out->kind = CARRIED_NIL;
        return true;
    }
    if (lhat_is_bool(value)) {
        out->kind = CARRIED_BOOL;
        out->as.boolean = lhat_as_bool(value);
        return true;
    }
    if (lhat_is_integer(value)) {
        out->kind = CARRIED_INT;
        out->as.integer = lhat_as_integer(value);
        return true;
    }
    if (lhat_is_real(value)) {
        out->kind = CARRIED_REAL;
        out->as.real = lhat_as_real(value);
        return true;
    }
    if (lhat_is_object_kind(value, LHAT_OBJECT_STRING)) {
        const LhatString *string = (const LhatString *)lhat_as_object(value);
        char *copy =
            string->length > 0 ? (char *)lhat_alloc(string->length) : NULL;
        if (string->length > 0 && copy == NULL) {
            return false;
        }
        if (string->length > 0) {
            memcpy(copy, string->text, string->length);
        }
        out->kind = CARRIED_STRING;
        out->as.text.bytes = copy;
        out->as.text.length = string->length;
        return true;
    }
    return false;  // a table, a closure, a hostdata value -- not carryable
}

static bool from_thread_value(LhatMachine *machine, const ThreadValue *carried,
                              LhatValue *out)
{
    switch (carried->kind) {
        case CARRIED_NIL:
            *out = lhat_nil();
            return true;
        case CARRIED_BOOL:
            *out = lhat_bool(carried->as.boolean);
            return true;
        case CARRIED_INT:
            *out = lhat_integer(carried->as.integer);
            return true;
        case CARRIED_REAL:
            *out = lhat_real(carried->as.real);
            return true;
        case CARRIED_STRING:
            return lhat_machine_make_string(machine, carried->as.text.bytes,
                                            carried->as.text.length, out);
    }
    return false;
}

static void free_thread_value(ThreadValue *carried)
{
    if (carried->kind == CARRIED_STRING) {
        lhat_free(carried->as.text.bytes);
    }
}

static void free_thread_values(ThreadValue *carried, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free_thread_value(&carried[i]);
    }
    lhat_free(carried);
}

// 実引数を新しいマシンの上に組み直す。組み立て途中の LhatValue はどの根から
// も辿れないが、収集が走るのは run_frames の命令境界だけ(src/gc.c)で、確保
// そのものは収集を起こさない -- だからこのループの間に回収されることはない。
static bool rebuild_arguments(LhatMachine *machine, const ThreadStart *start,
                              LhatValue **out)
{
    if (start->argument_count == 0) {
        *out = NULL;
        return true;
    }
    LhatValue *values =
        (LhatValue *)lhat_alloc(start->argument_count * sizeof *values);
    if (values == NULL) {
        return false;
    }
    for (size_t i = 0; i < start->argument_count; i++) {
        if (!from_thread_value(machine, &start->arguments[i], &values[i])) {
            lhat_free(values);
            return false;
        }
    }
    *out = values;
    return true;
}

static int thread_main(void *raw)
{
    ThreadStart *start = (ThreadStart *)raw;
    ThreadHandle *handle = start->handle;

    LhatMachine *machine = lhat_machine_new();
    if (machine == NULL) {
        handle->status = LHAT_RUN_OUT_OF_MEMORY;
    } else {
        lhat_machine_set_modules(machine, start->modules, start->module_count);

        LhatValue fn = lhat_nil();
        LhatValue *arguments = NULL;
        if (!lhat_machine_make_closure(machine, start->proto, &fn) ||
            !rebuild_arguments(machine, start, &arguments)) {
            handle->status = LHAT_RUN_OUT_OF_MEMORY;
        } else {
            // 13.7: fn takes one variadic slot, so lhat_machine_call is what
            // collects these into the table the body reads as '...'.
            LhatRunResult ran = lhat_machine_call(machine, fn, arguments,
                                                  start->argument_count);
            handle->status = ran.status;
            if (ran.status == LHAT_RUN_OK &&
                !to_thread_value(ran.value, &handle->result)) {
                handle->status = LHAT_RUN_TYPE_ERROR;  // 表現できない戻り値
            }
        }
        lhat_free(arguments);
        lhat_machine_dispose(machine);
    }

    free_thread_values(start->arguments, start->argument_count);
    lhat_free(start);
    return 0;
}

// join() を経由せず handle を手放す全ての経路(未 join の dispose()、
// spawn() の途中で hostdata の組み立てに失敗した場合)が通る、ここだけの
// 後始末。
//
// detach してすぐ忘れる形は採らない: 呼び出し元がこの ThreadHandle を
// dispose した直後に program/machine を破棄すると、まだ走っている
// thread_main は start->modules/proto の指す chunk(program が所有)を
// 読み続けており、use-after-free になる。「dispose はブロックしない」
// という利便性より、この安全性を優先する -- 実際 20 スレッドを join
// せず spawn→dispose するだけのテストで検証済みの実クラッシュだった。
static void join_and_free(ThreadHandle *handle)
{
    lhat_thread_join(&handle->os);
    if (handle->status == LHAT_RUN_OK) {
        free_thread_value(&handle->result);
    }
    lhat_free(handle);
}

// 13.7: the tail reaches an LhatHostFn uncollected (see LhatHost), so what
// spawn was written past fn is arguments[1..count) and nothing else. NULL out
// with a count of 0 when there was none.
//
// `type_refused` tells the two failures apart. to_thread_value answers false
// both for a value of a kind that does not cross and for a string it could not
// copy, and only the caller's error kind depends on which -- a string is the
// one carryable kind that allocates, so that is the whole test.
static bool carry_arguments(const LhatValue *arguments, size_t count,
                            ThreadValue **out, size_t *out_count,
                            bool *type_refused)
{
    *out = NULL;
    *out_count = 0;
    *type_refused = false;
    if (count <= 1) {
        return true;
    }
    size_t wanted = count - 1;
    ThreadValue *carried = (ThreadValue *)lhat_calloc(wanted, sizeof *carried);
    if (carried == NULL) {
        return false;
    }
    for (size_t i = 0; i < wanted; i++) {
        if (!to_thread_value(arguments[i + 1], &carried[i])) {
            *type_refused = !lhat_is_object_kind(arguments[i + 1],
                                                 LHAT_OBJECT_STRING);
            free_thread_values(carried, i);
            return false;
        }
    }
    *out = carried;
    *out_count = wanted;
    return true;
}

static LhatValue thread_spawn(LhatMachine *machine, void *context,
                              const LhatValue *arguments, size_t count)
{
    const ThreadModule *module = (const ThreadModule *)context;

    if (!lhat_is_object_kind(arguments[0], LHAT_OBJECT_SUBROUTINE)) {
        return fail_with(machine, module->not_spawnable, "not a subroutine");
    }
    const LhatClosure *closure =
        (const LhatClosure *)lhat_as_object(arguments[0]);
    // 13.7: one variadic slot and nothing else. A parameter of its own would
    // have a type spawn cannot promise to fill -- only the four kinds
    // to_thread_value carries reach the other machine, and '...' is the one
    // shape whose element type (any^) every one of them conforms to.
    if (closure->proto == NULL || lhat_proto_yields(closure->proto) ||
        lhat_proto_upvalue_count(closure->proto) != 0 ||
        !lhat_proto_has_variadic(closure->proto) ||
        lhat_proto_parameters(closure->proto) != 1) {
        return fail_with(machine, module->not_spawnable,
                         "fn closes over a variable, is yieldable, or does "
                         "not take '...' alone; spawn requires a plain "
                         "'p^...' closure with no captured places");
    }

    ThreadValue *carried = NULL;
    size_t carried_count = 0;
    bool type_refused = false;
    if (!carry_arguments(arguments, count, &carried, &carried_count,
                         &type_refused)) {
        return type_refused
                   ? fail_with(machine, module->bad_argument,
                               "an argument is not a nil^, bool^, number^ or "
                               "string^, and nothing else crosses to another "
                               "machine")
                   : fail_with(machine, module->out_of_memory, "out of memory");
    }

    const LhatModule *modules = NULL;
    size_t module_count = 0;
    lhat_machine_modules(machine, &modules, &module_count);

    ThreadHandle *handle = (ThreadHandle *)lhat_calloc(1, sizeof *handle);
    ThreadStart *start = (ThreadStart *)lhat_alloc(sizeof *start);
    if (handle == NULL || start == NULL) {
        free_thread_values(carried, carried_count);
        lhat_free(handle);
        lhat_free(start);
        return fail_with(machine, module->out_of_memory, "out of memory");
    }
    start->proto = closure->proto;
    start->modules = modules;
    start->module_count = module_count;
    start->arguments = carried;
    start->argument_count = carried_count;
    start->handle = handle;

    if (!lhat_thread_start(&handle->os, thread_main, start)) {
        free_thread_values(carried, carried_count);
        lhat_free(start);
        lhat_free(handle);
        return fail_with(machine, module->spawn_failed,
                         "the operating system refused to start a thread");
    }

    LhatValue out = lhat_nil();
    if (!lhat_machine_make_hostdata(machine, module->handle_tag, handle,
                                    &out)) {
        // The thread is already running with no handle left to give the
        // caller -- wait for it and free `handle` ourselves, the same as
        // an unjoined dispose() would.
        join_and_free(handle);
        return fail_with(machine, module->out_of_memory, "out of memory");
    }
    return out;
}

static LhatValue thread_join(LhatMachine *machine, void *context,
                             const LhatValue *arguments, size_t count)
{
    (void)count;
    const ThreadModule *module = (const ThreadModule *)context;
    ThreadHandle *handle =
        (ThreadHandle *)lhat_hostdata_pointer(arguments[0], module->handle_tag);
    if (handle == NULL || handle->joined) {
        return fail_with(machine, module->already_joined, "already joined");
    }

    lhat_thread_join(&handle->os);
    handle->joined = true;

    if (handle->status != LHAT_RUN_OK) {
        return fail_with(machine, module->bad_result,
                         lhat_run_status_message(handle->status));
    }
    LhatValue out = lhat_nil();
    if (!from_thread_value(machine, &handle->result, &out)) {
        return fail_with(machine, module->out_of_memory, "out of memory");
    }
    return out;
}

// 05 の 8.8: registering this is what makes a ThreadHandle the host's to
// hand over and L^'s to give back. A caller that already called join()
// leaves nothing more to wait for; one that never did makes dispose() do
// what join() would have (see join_and_free's comment for why detaching
// and moving on is not safe here).
static LhatValue thread_dispose(LhatMachine *machine, void *context,
                                const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)count;
    const ThreadModule *module = (const ThreadModule *)context;
    ThreadHandle *handle =
        (ThreadHandle *)lhat_hostdata_pointer(arguments[0], module->handle_tag);
    if (handle == NULL) {
        return lhat_nil();
    }
    if (handle->joined) {
        if (handle->status == LHAT_RUN_OK) {
            free_thread_value(&handle->result);
        }
        lhat_free(handle);
    } else {
        join_and_free(handle);
    }
    return lhat_nil();
}

bool lhatstdlib_thread_register(LhatProgram *program)
{
    // 05 の 8.7: 登録は検査より前 -- std.error.OutOfMemory を使う側(この
    // モジュール)より前に確実に存在させる。lhatstdlib_error_register は
    // 冪等なので、他の stdlib モジュールが先に呼んでいても構わない。
    if (!lhatstdlib_error_register(program)) {
        return false;
    }

    ThreadModule *module = (ThreadModule *)lhat_calloc(1, sizeof *module);
    if (module == NULL) {
        return false;
    }
    module->out_of_memory = lhatstdlib_error_lookup(program, "OutOfMemory");

    static const char *const variants[] = {"NotSpawnable", "BadArgument",
                                           "SpawnFailed", "BadResult",
                                           "AlreadyJoined"};
    const LhatErrorKind *kinds[5];
    if (!lhat_register_error_kind(program, "std.thread", "ThreadError",
                                  variants, 5, NULL, kinds)) {
        lhat_free(module);
        return false;
    }
    module->not_spawnable = kinds[0];
    module->bad_argument = kinds[1];
    module->spawn_failed = kinds[2];
    module->bad_result = kinds[3];
    module->already_joined = kinds[4];

    module->handle_tag =
        lhat_register_hostdata_type(program, "std.thread", "ThreadHandle");
    if (module->handle_tag == NULL) {
        lhat_free(module);
        return false;
    }

    // 13.7: fn's own '...' takes any^ underneath, so a plain 'p^ ... { }' is
    // the closure this signature asks for (conformance on a variadic element
    // is contravariant, the same as an ordinary parameter's).
    //
    // spawn's own '...' is any^ for the same reason read the other way round.
    // Naming the four carryable kinds here would read better, and would also
    // make the one call this feature exists for -- forwarding a caller's own
    // collector, 'spawn(fn, ...)' -- impossible: that collector is any^ and
    // any^ conforms to no narrower type (02 の 13.7). So the boundary is stated at
    // run time instead, by ThreadError.BadArgument, and the signature asks
    // only what it can ask without shutting the door on a spread.
    //
    // The result is not widened the same way: it is copied back rather than
    // forwarded, nothing has to fit through a variadic slot on the way, and
    // the checker settles it outright.
    return lhat_register_func(
               program, "std.thread", "spawn",
               "f^p^... -> number^|bool^|string^|nil^;, ...:any^ -> "
               "std.thread.ThreadHandle|std.thread.ThreadError.NotSpawnable"
               "|std.thread.ThreadError.BadArgument"
               "|std.thread.ThreadError.SpawnFailed|std.error.OutOfMemory;",
               thread_spawn, module) &&
           lhat_register_member(
               program, "std.thread", "ThreadHandle", "join",
               "f^self^ -> (number^|bool^|string^|nil^)"
               "|std.thread.ThreadError.AlreadyJoined"
               "|std.thread.ThreadError.BadResult|std.error.OutOfMemory;",
               thread_join, module) &&
           lhat_register_member(program, "std.thread", "ThreadHandle",
                                "dispose", "p^self^;", thread_dispose,
                                module);
}
