// L^ (lhat) -- sample standard library: std.thread.
//
// spawn(fn) starts fn -- p^ -> (number^|bool^|string^|nil^); with no
// upvalues -- on a fresh LhatMachine of its own, on a new OS thread. join()
// blocks for the result and dispose() (05 の 8.8) blocks the same way if the
// caller never joined -- see thread_dispose's comment for why letting an
// unjoined thread run past its ThreadHandle's disposal is not safe here.
//
// A Machine/its GC heap is one OS thread's alone (src/gc.c's "one set of
// roots rather than a thread apiece"), so a value cannot simply be handed to
// another machine -- it would still point into the first one's heap.
// Everything that crosses the boundary here is copied into a plain C
// representation first (ThreadResult) and only turned back into an LhatValue
// on the machine that is going to use it. That is also why spawn takes no
// arguments: a table, a closure or a hostdata value has no such plain
// representation, and there is no host API to build a table's contents from
// C, so carrying one across is not attempted by this sample. A closure's
// Proto, unlike its upvalues, is safe to share -- see lhat_proto_new's
// comment on why it is always born already black.
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
    const LhatErrorKind *spawn_failed;
    const LhatErrorKind *bad_result;
    const LhatErrorKind *already_joined;
    const LhatErrorKind *out_of_memory;  // std.error.OutOfMemory -- error.h
    const LhatHostDataTag *handle_tag;
} ThreadModule;

typedef enum {
    RESULT_NIL,
    RESULT_BOOL,
    RESULT_INT,
    RESULT_REAL,
    RESULT_STRING
} ResultKind;

typedef struct {
    ResultKind kind;
    union {
        bool boolean;
        int64_t integer;
        double real;
        struct {
            char *bytes;
            size_t length;
        } text;
    } as;
} ThreadResult;

typedef struct {
    const LhatProto *proto;      // borrowed; lives as long as the program
    const LhatModule *modules;   // borrowed; same
    size_t module_count;
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
    ThreadResult result;
} ThreadHandle;

static LhatValue fail_with(LhatMachine *machine, const LhatErrorKind *kind,
                           const char *message)
{
    LhatValue error = lhat_nil();
    return lhat_machine_make_error(machine, kind, message, lhat_nil(), &error)
               ? error
               : lhat_nil();
}

static bool to_thread_result(LhatValue value, ThreadResult *out)
{
    if (lhat_is_nil(value)) {
        out->kind = RESULT_NIL;
        return true;
    }
    if (lhat_is_bool(value)) {
        out->kind = RESULT_BOOL;
        out->as.boolean = lhat_as_bool(value);
        return true;
    }
    if (lhat_is_integer(value)) {
        out->kind = RESULT_INT;
        out->as.integer = lhat_as_integer(value);
        return true;
    }
    if (lhat_is_real(value)) {
        out->kind = RESULT_REAL;
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
        out->kind = RESULT_STRING;
        out->as.text.bytes = copy;
        out->as.text.length = string->length;
        return true;
    }
    return false;  // a table, a closure, a hostdata value -- not carryable
}

static bool from_thread_result(LhatMachine *machine,
                               const ThreadResult *result, LhatValue *out)
{
    switch (result->kind) {
        case RESULT_NIL:
            *out = lhat_nil();
            return true;
        case RESULT_BOOL:
            *out = lhat_bool(result->as.boolean);
            return true;
        case RESULT_INT:
            *out = lhat_integer(result->as.integer);
            return true;
        case RESULT_REAL:
            *out = lhat_real(result->as.real);
            return true;
        case RESULT_STRING:
            return lhat_machine_make_string(machine, result->as.text.bytes,
                                            result->as.text.length, out);
    }
    return false;
}

static void free_thread_result(ThreadResult *result)
{
    if (result->kind == RESULT_STRING) {
        lhat_free(result->as.text.bytes);
    }
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
        if (!lhat_machine_make_closure(machine, start->proto, &fn)) {
            handle->status = LHAT_RUN_OUT_OF_MEMORY;
        } else {
            LhatRunResult ran = lhat_machine_call(machine, fn, NULL, 0);
            handle->status = ran.status;
            if (ran.status == LHAT_RUN_OK &&
                !to_thread_result(ran.value, &handle->result)) {
                handle->status = LHAT_RUN_TYPE_ERROR;  // 表現できない戻り値
            }
        }
        lhat_machine_dispose(machine);
    }

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
        free_thread_result(&handle->result);
    }
    lhat_free(handle);
}

static LhatValue thread_spawn(LhatMachine *machine, void *context,
                              const LhatValue *arguments, size_t count)
{
    (void)count;
    const ThreadModule *module = (const ThreadModule *)context;

    if (!lhat_is_object_kind(arguments[0], LHAT_OBJECT_SUBROUTINE)) {
        return fail_with(machine, module->not_spawnable, "not a subroutine");
    }
    const LhatClosure *closure =
        (const LhatClosure *)lhat_as_object(arguments[0]);
    if (closure->proto == NULL || closure->proto->yields ||
        closure->proto->upvalue_count != 0) {
        return fail_with(machine, module->not_spawnable,
                         "fn closes over a variable, or is yieldable; "
                         "spawn requires a plain closure with no captured "
                         "places");
    }

    const LhatModule *modules = NULL;
    size_t module_count = 0;
    lhat_machine_modules(machine, &modules, &module_count);

    ThreadHandle *handle = (ThreadHandle *)lhat_calloc(1, sizeof *handle);
    ThreadStart *start = (ThreadStart *)lhat_alloc(sizeof *start);
    if (handle == NULL || start == NULL) {
        lhat_free(handle);
        lhat_free(start);
        return fail_with(machine, module->out_of_memory, "out of memory");
    }
    start->proto = closure->proto;
    start->modules = modules;
    start->module_count = module_count;
    start->handle = handle;

    if (!lhat_thread_start(&handle->os, thread_main, start)) {
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
    if (!from_thread_result(machine, &handle->result, &out)) {
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
            free_thread_result(&handle->result);
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

    static const char *const variants[] = {"NotSpawnable", "SpawnFailed",
                                           "BadResult", "AlreadyJoined"};
    const LhatErrorKind *kinds[4];
    if (!lhat_register_error_kind(program, "std.thread", "ThreadError",
                                  variants, 4, NULL, kinds)) {
        lhat_free(module);
        return false;
    }
    module->not_spawnable = kinds[0];
    module->spawn_failed = kinds[1];
    module->bad_result = kinds[2];
    module->already_joined = kinds[3];

    module->handle_tag =
        lhat_register_hostdata_type(program, "std.thread", "ThreadHandle");
    if (module->handle_tag == NULL) {
        lhat_free(module);
        return false;
    }

    return lhat_register_func(
               program, "std.thread", "spawn",
               "f^p^ -> number^|bool^|string^|nil^; -> "
               "std.thread.ThreadHandle|std.thread.ThreadError.NotSpawnable"
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
