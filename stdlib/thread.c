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
// Everything that crosses the boundary here is taken apart into a form that
// belongs to neither machine (carry.h's LhatCarried) and put back together
// on the machine that is going to use it. The same copy runs both ways: fn
// and the arguments on the way in, the answer on the way back.
//
// What that copy can carry is what carry.h says -- the primitives, tables
// (cycles kept), and closures as their shared proto plus a snapshot of what
// they closed over. It is still the whole of why fn is written 'p^...'
// rather than with parameters of its own: 13.7's collector is any^
// underneath, which is the one shape every carried value conforms to
// without a written parameter having to name it. What crosses is checked as
// the call is made, not before it: an argument carry refuses answers
// ThreadError.BadArgument with carry's reason. See the registration at the
// foot of this file for why that is a run-time answer and not a type.
//
// spawn/join answer a ThreadHandle or a ThreadError, or std.error.
// OutOfMemory for the one failure this module shares with every other
// stdlib module (see error.h) -- read the result with try^/catch^, or
// narrow with isa^ against std.thread.ThreadHandle (05 の 8.8's hostdata
// path).

#include "carry.h"
#include "error.h"
#include "thread.h"

#include "port/thread.h"

#include <limits.h>
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
    // 05 の 8.7: what the host registered, so that a machine started here can
    // be given its own copies. Borrowed -- a program outlives every thread
    // started from it (join_and_free's comment says why that is not a hope).
    const LhatProgram *program;
    const LhatErrorKind *not_spawnable;
    const LhatErrorKind *bad_argument;
    const LhatErrorKind *spawn_failed;
    const LhatErrorKind *bad_result;
    const LhatErrorKind *already_joined;
    const LhatErrorKind *out_of_memory;  // std.error.OutOfMemory -- error.h
    const LhatHostDataTag *handle_tag;
} ThreadModule;

typedef struct {
    // fn itself, carried (carry.h): its proto is shared and its captured
    // places arrive as copies -- so a closure that closes over something
    // crosses too, as a snapshot of what it closed over.
    LhatCarried *fn;
    const LhatModule *modules;   // borrowed; lives as long as the program
    size_t module_count;
    const LhatProgram *program;  // borrowed; same. What thread_main installs
    // 13.7's collector, still in the form that crossed. Owned by this, and
    // given back by thread_main as soon as the new machine has its own copy.
    LhatCarried **arguments;
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
    LhatCarried *result;  // NULL until the body answered
    // 04 の 11.6改: the worker's frames, rendered before its machine went
    // -- the joiner's error message carries them. NULL when the run was
    // clean or nothing was standing. Owned here, freed with the handle.
    char *traceback;
    // 02 の 15.14: what a scheduler asks instead of waiting -- has the body
    // finished. This one *is* read while the thread may still be running, so
    // unlike the fields above it is not covered by the join's ordering and
    // takes a lock of its own. The lock is held for a bool and nothing else.
    LhatMutex done_lock;
    bool finished;
} ThreadHandle;

// The last thing thread_main does, and the only thing done() reads.
static void mark_finished(ThreadHandle *handle)
{
    lhat_mutex_lock(&handle->done_lock);
    handle->finished = true;
    lhat_mutex_unlock(&handle->done_lock);
}

static bool has_finished(ThreadHandle *handle)
{
    lhat_mutex_lock(&handle->done_lock);
    bool finished = handle->finished;
    lhat_mutex_unlock(&handle->done_lock);
    return finished;
}

static LhatValue fail_with(LhatMachine *machine, const LhatErrorKind *kind,
                           const char *message)
{
    LhatValue error = lhat_nil();
    return lhat_machine_make_error(machine, kind, message, lhat_nil(), &error)
               ? error
               : lhat_nil();
}

static void free_carried_values(LhatCarried **carried, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        lhat_carried_free(carried[i]);
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
        if (!lhat_uncarry(machine, start->arguments[i], &values[i])) {
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
        // 05 の 8.7: a registration becomes an object on the heap of the
        // machine it is installed on, so L^ and L^.modules are empty here
        // until this runs -- and the body was compiled against them. Reading
        // the program is all this does; several threads may be installing at
        // once, each onto a machine of its own.
        //
        // What a registration was handed as its `context` is the one thing
        // shared between them (see thread.h).
        if (!lhat_program_install(start->program, machine) ||
            !lhat_uncarry(machine, start->fn, &fn) ||
            !rebuild_arguments(machine, start, &arguments)) {
            handle->status = LHAT_RUN_OUT_OF_MEMORY;
        } else {
            // 13.7: fn takes one variadic slot, so lhat_machine_call is what
            // collects these into the table the body reads as '...'.
            LhatRunResult ran = lhat_machine_call(machine, fn, arguments,
                                                  start->argument_count);
            handle->status = ran.status;
            if (ran.status == LHAT_RUN_OK &&
                !lhat_carry(ran.value, &handle->result, NULL)) {
                handle->status = LHAT_RUN_TYPE_ERROR;  // 表現できない戻り値
            }
            // 04 の 11.6改: the frames are about to go with the machine, so
            // the text is made now for the join to carry.
            if (ran.status != LHAT_RUN_OK &&
                lhat_machine_fault_depth(machine) >= 2) {
                size_t needed = lhat_machine_traceback(machine, NULL, 0);
                handle->traceback = (char *)lhat_alloc(needed + 1);
                if (handle->traceback != NULL) {
                    lhat_machine_traceback(machine, handle->traceback,
                                           needed + 1);
                }
            }
        }
        lhat_free(arguments);
        lhat_machine_dispose(machine);
    }

    free_carried_values(start->arguments, start->argument_count);
    lhat_carried_free(start->fn);
    lhat_free(start);
    // Last of all: everything the join will read is written by now, so a
    // done() that sees this can be followed by a join that does not wait.
    mark_finished(handle);
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
        lhat_carried_free(handle->result);
    }
    lhat_mutex_destroy(&handle->done_lock);
    lhat_free(handle->traceback);
    lhat_free(handle);
}

// 13.7: the tail reaches an LhatHostFn uncollected (see LhatHost), so what
// spawn was written past fn is arguments[1..count) and nothing else. NULL out
// with a count of 0 when there was none.
//
// `refused` carries carry.h's reason when a value cannot cross, and stays
// NULL for the one other failure, running out of memory.
static bool carry_arguments(const LhatValue *arguments, size_t count,
                            LhatCarried ***out, size_t *out_count,
                            const char **refused)
{
    *out = NULL;
    *out_count = 0;
    *refused = NULL;
    if (count <= 1) {
        return true;
    }
    size_t wanted = count - 1;
    LhatCarried **carried =
        (LhatCarried **)lhat_calloc(wanted, sizeof *carried);
    if (carried == NULL) {
        return false;
    }
    for (size_t i = 0; i < wanted; i++) {
        const char *why = NULL;
        if (!lhat_carry(arguments[i + 1], &carried[i], &why)) {
            if (why != NULL && strcmp(why, "out of memory") != 0) {
                *refused = why;
            }
            free_carried_values(carried, i);
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
    const LhatProto *proto = lhat_closure_proto(arguments[0]);
    // 13.7: one variadic slot and nothing else. A parameter of its own would
    // have a type spawn cannot promise to fill -- what carry.h carries is
    // a snapshot of the value, and '...' is the one shape whose element
    // type (any^) every one of them conforms to. What fn closes over
    // crosses with it, as a copy (carry.h); a capture that cannot cross is
    // what carry refuses below.
    if (proto == NULL || lhat_proto_yields(proto) ||
        !lhat_proto_has_variadic(proto) || lhat_proto_parameters(proto) != 1) {
        return fail_with(machine, module->not_spawnable,
                         "fn is yieldable or does not take '...' alone; "
                         "spawn requires a 'p^...' closure");
    }
    const char *fn_refused = NULL;
    LhatCarried *fn = NULL;
    if (!lhat_carry(arguments[0], &fn, &fn_refused)) {
        return fn_refused != NULL && strcmp(fn_refused, "out of memory") != 0
                   ? fail_with(machine, module->not_spawnable, fn_refused)
                   : fail_with(machine, module->out_of_memory, "out of memory");
    }

    LhatCarried **carried = NULL;
    size_t carried_count = 0;
    const char *refused = NULL;
    if (!carry_arguments(arguments, count, &carried, &carried_count,
                         &refused)) {
        lhat_carried_free(fn);
        return refused != NULL
                   ? fail_with(machine, module->bad_argument, refused)
                   : fail_with(machine, module->out_of_memory, "out of memory");
    }

    const LhatModule *modules = NULL;
    size_t module_count = 0;
    lhat_machine_modules(machine, &modules, &module_count);

    ThreadHandle *handle = (ThreadHandle *)lhat_calloc(1, sizeof *handle);
    ThreadStart *start = (ThreadStart *)lhat_alloc(sizeof *start);
    if (handle == NULL || start == NULL) {
        free_carried_values(carried, carried_count);
        lhat_carried_free(fn);
        lhat_free(handle);
        lhat_free(start);
        return fail_with(machine, module->out_of_memory, "out of memory");
    }
    lhat_mutex_init(&handle->done_lock);
    start->fn = fn;
    start->modules = modules;
    start->module_count = module_count;
    start->program = module->program;
    start->arguments = carried;
    start->argument_count = carried_count;
    start->handle = handle;

    if (!lhat_thread_start(&handle->os, thread_main, start)) {
        free_carried_values(carried, carried_count);
        lhat_carried_free(fn);
        lhat_free(start);
        lhat_mutex_destroy(&handle->done_lock);
        lhat_free(handle->traceback);
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
        // 04 の 11.6改: the worker's own frames ride the message -- the one
        // channel that crosses machines here.
        const char *said = lhat_run_status_message(handle->status);
        if (handle->traceback != NULL) {
            size_t said_length = strlen(said);
            size_t trace_length = strlen(handle->traceback);
            char *joined_text =
                (char *)lhat_alloc(said_length + 1 + trace_length + 1);
            if (joined_text != NULL) {
                memcpy(joined_text, said, said_length);
                joined_text[said_length] = '\n';
                memcpy(joined_text + said_length + 1, handle->traceback,
                       trace_length + 1);
                LhatValue answered =
                    fail_with(machine, module->bad_result, joined_text);
                lhat_free(joined_text);
                return answered;
            }
        }
        return fail_with(machine, module->bad_result, said);
    }
    LhatValue out = lhat_nil();
    if (handle->result != NULL &&
        !lhat_uncarry(machine, handle->result, &out)) {
        return fail_with(machine, module->out_of_memory, "out of memory");
    }
    return out;
}

// The one registration here that starts no thread and holds no handle: it
// stops the thread that called it. Written in seconds because that is what a
// caller has in mind ('sleep(0.2)'); port/thread.h counts in milliseconds, so
// the conversion happens here and nowhere else.
static LhatValue thread_sleep(LhatMachine *machine, void *context,
                              const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    (void)count;
    double seconds = lhat_is_integer(arguments[0])
                         ? (double)lhat_as_integer(arguments[0])
                         : lhat_as_real(arguments[0]);
    // Compared the way round that answers for a NaN as well: nothing to wait
    // for is not an error, the same line 14.19 draws for a range that does
    // not hold. The ceiling is what an int carries -- about 24 days.
    double milliseconds = seconds * 1000.0;
    int wait = 0;
    if (milliseconds > 0.0) {
        wait = milliseconds >= (double)INT_MAX ? INT_MAX : (int)milliseconds;
    }
    lhat_thread_sleep(wait);
    return lhat_nil();
}

// 02 の 15.14: the question a scheduler asks in place of waiting. join()
// blocks, which is the one thing a loop with other tasks to run may not do --
// so this answers whether the join would return at once, and a task awaiting
// a thread is written as "ask, and if not, await a short delay".
//
// Answering true says the body has finished, and every field the join reads
// was written before the flag was (thread_main sets it last). A false is only
// ever "not yet as of now": the thread may finish in the next instant, which
// is why the shape above loops rather than deciding anything on one answer.
static LhatValue thread_done(LhatMachine *machine, void *context,
                             const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)count;
    const ThreadModule *module = (const ThreadModule *)context;
    ThreadHandle *handle =
        (ThreadHandle *)lhat_hostdata_pointer(arguments[0], module->handle_tag);
    // A handle already joined has nothing left to run, and one that is not a
    // handle of this program answers the same way an absent member would.
    return lhat_bool(handle != NULL && (handle->joined || has_finished(handle)));
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
            lhat_carried_free(handle->result);
        }
        lhat_mutex_destroy(&handle->done_lock);
        lhat_free(handle->traceback);
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
    module->program = program;
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

    // 13.7: fn's own '...' takes any^ underneath, so a 'p^ ... { }' is the
    // closure this signature asks for (conformance on a variadic element is
    // contravariant, the same as an ordinary parameter's). What it closes
    // over crosses with it as a snapshot (carry.h), so 15.13's closed^ is
    // no longer asked for -- a closed^ one still fits, promising more.
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
               "f^p^... -> any^;, ...:any^ -> "
               "std.thread.ThreadHandle|std.thread.ThreadError.NotSpawnable"
               "|std.thread.ThreadError.BadArgument"
               "|std.thread.ThreadError.SpawnFailed|std.error.OutOfMemory;",
               thread_spawn, module) &&
           // 13.4 keeps the name out of a signature, so what the number^ is
           // counted in is said here and in thread.h: seconds.
           lhat_register_func(program, "std.thread", "sleep", "p^number^;",
                              thread_sleep, NULL) &&
           lhat_register_member(
               program, "std.thread", "ThreadHandle", "join",
               "f^self^ -> (number^|bool^|string^|nil^)"
               "|std.thread.ThreadError.AlreadyJoined"
               "|std.thread.ThreadError.BadResult|std.error.OutOfMemory;",
               thread_join, module) &&
           lhat_register_member(program, "std.thread", "ThreadHandle", "done",
                                "f^self^ -> bool^;", thread_done, module) &&
           lhat_register_member(program, "std.thread", "ThreadHandle",
                                "dispose", "p^self^;", thread_dispose,
                                module);
}
