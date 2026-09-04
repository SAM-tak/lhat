// L^ (lhat) -- sample standard library: std.task (see task.h).
//
// A MACHINE PER TASK, and N OS threads that take turns running them.
// Everything that crosses goes through carry.h, so nothing here reaches into
// another machine's heap.
//
// A machine is what a task's whole state is. Its frame stack is one stack
// and a coroutine saves one frame, so a single machine cannot hold two jobs
// half-done -- which is why a task that stops in the middle of a call chain
// needs a machine of its own to stop in. 03 の 4.3改 is what made that
// affordable: a task's machine is measured for the job rather than for a
// REPL.
//
// The worker owns no machine at all. It takes a runnable task, gives it one
// slice (02 の 15.15), and files it by what the slice answered:
//
//   still going  -> the back of the run queue, so the next task gets a turn
//   waiting      -> the parked set, and the worker takes something else
//   done, failed -> the answer is carried out and the machine is let go
//
// The middle line is the point. A job that waits on std.async used to hold
// its worker asleep; now it holds nothing, and N threads keep working
// however many tasks are parked.

#include "task.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "async.h"
#include "carry.h"
#include "error.h"
#include "lhat/port.h"
#include "port/thread.h"

typedef struct Task Task;

// 02 の 15.15: how many loop turns a task runs before the worker takes it
// off. This is the quantum -- what one task may do before another gets a
// turn -- and it is also what lets a body that neither yields nor ends be
// put down at all. Small enough that a stop is answered in well under a
// millisecond and a spinning task does not starve its neighbours, large
// enough that the counting costs nothing.
#define TASK_SLICE 20000

// 03 の 4.3改: how big a task's machine is. It is PER TASK now, so it is
// paid by everything running or parked at once rather than once per worker
// -- which is what makes the measurements worth having. The default ones
// (about 160 KiB) would make a thousand parked tasks 160 MiB; these make it
// 48 MiB.
//
// It is a depth of 64 rather than 200, and a job that recurses past it ends
// with LHAT_RUN_STACK_OVERFLOW and comes back through Task.failed() like any
// other failure. A job that needs more depth than that is a job std.thread
// should run.
#define TASK_FRAMES 64
#define TASK_SLOTS 2048

typedef struct {
    const LhatProgram *program;  // borrowed; the workers install it
    const LhatErrorKind *not_started;
    const LhatErrorKind *refused;
    const LhatErrorKind *failed;
    const LhatErrorKind *out_of_memory;
    const LhatHostDataTag *tag;

    // The pool. `lock` covers the queue and every field below it; `work`
    // wakes a worker when a job arrives or the pool is told to stop.
    LhatMutex lock;
    LhatCondition work;
    bool running;
    LhatThread *threads;
    size_t worker_count;

    Task **queue;  // a ring of tasks that could run right now
    size_t head;
    size_t queued;
    size_t capacity;

    // Tasks parked on a std.async wait. Nothing signals when one of these
    // becomes ready (a timer's clock, a push from another thread), so a
    // worker with nothing runnable is what looks -- see move_ready.
    Task **parked;
    size_t parked_count;
    size_t parked_capacity;
    int64_t looked_at;  // when a worker last looked them over
} TaskModule;

// What the worker does to a task next. A task carries this between turns
// because the turns are on different threads.
typedef enum {
    MOVE_START,     // nothing has run; the machine is not made yet
    MOVE_RESUME,    // `driving` is a coroutine with more to do
    MOVE_CONTINUE,  // the last slice ran out of budget mid-instruction
    MOVE_NONE       // finished, one way or the other
} TaskMove;

// What a turn answered, which is how the worker files the task.
typedef enum { STEP_AGAIN, STEP_PARK, STEP_DONE } StepResult;

struct Task {
    TaskModule *module;

    // 03 の 4.3改: the task's own machine, made the first time it runs and
    // let go when it ends. Only ever touched by the worker holding the task
    // -- a task is in the run queue, in the parked set, or out with exactly
    // one worker, never two of those.
    LhatMachine *machine;
    LhatValue driving;  // the coroutine being resumed, once there is one
    bool driving_set;
    LhatRunResult ran;  // what the last turn answered
    TaskMove move;
    int64_t parked_on;  // the std.async id it is parked on

    // What to run: the job, and the arguments where it is a closure. Owned
    // here until a worker has rebuilt them.
    LhatCarried *job;
    LhatCarried **arguments;
    size_t argument_count;

    // What came of it. Written by the worker under `lock`, which is also
    // what `await` waits on.
    LhatMutex lock;
    LhatCondition done;
    bool finished;
    LhatRunStatus status;
    LhatCarried *result;
    char *traceback;
    // 04 の 11.6改: what it failed with and where, read on the worker while
    // its machine is still standing (stdlib/thread.c says why).
    char *fault_text;
    uint32_t fault_line;

    // 15.14改: the wait a scheduler parks on, pushed when the job ends.
    void *waits;
    int64_t await_id;

    // 05 の 8.8改2: a wrapper on some machine, the queue, the worker
    // running it. The last to let go frees it.
    int holds;
};

// ---------------------------------------------------------------------------
// A task, which knows nothing about L^
// ---------------------------------------------------------------------------

static void free_carried_values(LhatCarried **values, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        lhat_carried_free(values[i]);
    }
    lhat_free(values);
}

static void task_free(Task *task)
{
    // A task dropped where it stood (the pool was torn down under it) still
    // has its frames up. The machine goes whole, as 02 の 15.15 says a run
    // that will not be continued may.
    if (task->machine != NULL) {
        lhat_machine_dispose(task->machine);
    }
    lhat_carried_free(task->job);
    free_carried_values(task->arguments, task->argument_count);
    lhat_carried_free(task->result);
    lhat_free(task->traceback);
    lhat_free(task->fault_text);
    lhat_condition_destroy(&task->done);
    lhat_mutex_destroy(&task->lock);
    lhat_free(task);
}

static void task_retain(void *pointer, void *context)
{
    Task *task = (Task *)pointer;
    (void)context;
    lhat_mutex_lock(&task->lock);
    task->holds++;
    lhat_mutex_unlock(&task->lock);
}

static void task_let_go(void *pointer, void *context)
{
    Task *task = (Task *)pointer;
    (void)context;
    lhat_mutex_lock(&task->lock);
    bool last = --task->holds <= 0;
    lhat_mutex_unlock(&task->lock);
    if (last) {
        task_free(task);
    }
}

// The end of a job, whichever way it ended: what await was waiting for, and
// the push a scheduler parked on.
static void task_finished(Task *task)
{
    lhat_mutex_lock(&task->lock);
    task->finished = true;
    void *waits = task->waits;
    int64_t id = task->await_id;
    lhat_condition_broadcast(&task->done);
    lhat_mutex_unlock(&task->lock);
    if (id != 0) {
        lhatstdlib_async_complete(waits, id);
    }
}

// 04 の 11.6改: what a failure reads as. The status names the kind of it,
// and the three things beside it are what a reader actually needs: WHAT was
// panicked with, WHERE it happened, and the frames it was standing in.
//
// The value is rendered on the worker's own thread while its machine is
// still there (run_job below) -- it is that machine's object, and
// nothing of it survives the dispose. Without that, a body whose whole
// failure is `panic^ "..."` reported the one word `panic^`, which says
// neither what nor where (the same hole stdlib/thread.c had).
static char *failure_text(const Task *task)
{
    if (task->status == LHAT_RUN_OK) {
        return NULL;
    }
    const char *said = lhat_run_status_message(task->status);
    const char *value = task->fault_text;
    const char *trace = task->traceback;
    char where[32];
    where[0] = '\0';
    if (task->fault_line > 0) {
        snprintf(where, sizeof where, " (line %u)",
                 (unsigned)task->fault_line);
    }
    size_t needed = strlen(said) + (value != NULL ? 2 + strlen(value) : 0) +
                    strlen(where) + (trace != NULL ? 1 + strlen(trace) : 0);
    char *text = (char *)lhat_alloc(needed + 1);
    if (text == NULL) {
        return NULL;
    }
    snprintf(text, needed + 1, "%s%s%s%s%s%s", said,
             value != NULL ? ": " : "", value != NULL ? value : "", where,
             trace != NULL ? "\n" : "", trace != NULL ? trace : "");
    return text;
}

// ---------------------------------------------------------------------------
// The queue, and the worker that reads it
// ---------------------------------------------------------------------------

// `module->lock` is held.
static bool queue_push(TaskModule *module, Task *task)
{
    if (module->queued == module->capacity) {
        size_t wanted = module->capacity > 0 ? module->capacity * 2 : 8;
        Task **bigger = (Task **)lhat_calloc(wanted, sizeof *bigger);
        if (bigger == NULL) {
            return false;
        }
        for (size_t i = 0; i < module->queued; i++) {
            bigger[i] = module->queue[(module->head + i) % module->capacity];
        }
        lhat_free(module->queue);
        module->queue = bigger;
        module->capacity = wanted;
        module->head = 0;
    }
    module->queue[(module->head + module->queued) % module->capacity] = task;
    module->queued++;
    lhat_condition_signal(&module->work);
    return true;
}

// `module->lock` is held. NULL when there is nothing waiting.
static Task *queue_pop(TaskModule *module)
{
    if (module->queued == 0) {
        return NULL;
    }
    Task *task = module->queue[module->head];
    module->head = (module->head + 1) % module->capacity;
    module->queued--;
    return task;
}

// Whether the pool is still taking work. Read between slices, which is how
// a job that never yields still lets a stop through.
static bool pool_running(TaskModule *module)
{
    lhat_mutex_lock(&module->lock);
    bool running = module->running;
    lhat_mutex_unlock(&module->lock);
    return running;
}

// 04 の 11.6改: what a run failed with, kept while the machine that ran it
// is still standing -- the value is one of its objects and the frames are
// still up. Every way out of run_job that carries a fault comes through
// here, which is what the closure path used to skip.
static void note_failure(Task *task, LhatMachine *machine, LhatRunResult ran)
{
    task->status = ran.status;
    if (ran.status == LHAT_RUN_OK) {
        return;
    }
    task->fault_line = ran.line;
    if (!lhat_is_nil(ran.value)) {
        size_t room = lhat_value_write(ran.value, NULL, 0);
        task->fault_text = (char *)lhat_alloc(room + 1);
        if (task->fault_text != NULL) {
            lhat_value_write(ran.value, task->fault_text, room + 1);
        }
    }
    if (lhat_machine_fault_depth(machine) >= 2) {
        size_t needed = lhat_machine_traceback(machine, NULL, 0);
        task->traceback = (char *)lhat_alloc(needed + 1);
        if (task->traceback != NULL) {
            lhat_machine_traceback(machine, task->traceback, needed + 1);
        }
    }
}

// ---------------------------------------------------------------------------
// One task, one turn at a time
// ---------------------------------------------------------------------------

// Reads what the turn answered and says how the worker should file the task,
// leaving `move` set to whatever the next turn does.
static StepResult after_turn(TaskModule *module, Task *task)
{
    // 02 の 15.15: the budget ran out mid-body. The frames are standing and
    // lhat_machine_continue picks them up -- on whichever worker takes the
    // task next, since a machine has no thread of its own.
    if (task->ran.status == LHAT_RUN_SUSPENDED) {
        task->move = MOVE_CONTINUE;
        return STEP_AGAIN;
    }
    if (task->ran.status != LHAT_RUN_OK) {
        note_failure(task, task->machine, task->ran);
        task->move = MOVE_NONE;
        return STEP_DONE;
    }
    // A closure's call answered. 02 の 15.5: a yieldable one answers its
    // coroutine, and that is what the rest of this drives; anything else is
    // the job's own answer.
    if (!task->driving_set) {
        if (lhat_is_object_kind(task->ran.value, LHAT_OBJECT_COROUTINE)) {
            task->driving = task->ran.value;
            task->driving_set = true;
            task->move = MOVE_RESUME;
            return STEP_AGAIN;
        }
        task->status = LHAT_RUN_OK;
        task->move = MOVE_NONE;
        return STEP_DONE;
    }
    if (lhat_machine_coroutine_done(task->driving)) {
        task->status = LHAT_RUN_OK;
        task->move = MOVE_NONE;
        return STEP_DONE;
    }

    // It yielded. What it handed over is read the way sample/async.lh's
    // Scheduler reads it: a std.async id of THIS machine's is something to
    // wait for, and anything else -- 0, another number, a value of another
    // kind -- is a body that stopped to hand something over rather than to
    // wait, so it goes straight on.
    //
    // 05 の 8.7改: taken by id rather than by whatever is ready, which is
    // what keeps two tasks from taking each other's.
    task->move = MOVE_RESUME;
    int64_t id =
        lhat_is_integer(task->ran.value) ? lhat_as_integer(task->ran.value) : 0;
    void *waits = lhatstdlib_async_waits(module->program);
    if (waits == NULL || id <= 0) {
        return STEP_AGAIN;
    }
    // 1 says it was ready and is now taken; -1 says this machine armed no
    // such wait, so the number was not one. Either way the job runs on. 0 is
    // the only answer that parks it.
    if (lhatstdlib_async_take(waits, task->machine, id) != 0) {
        return STEP_AGAIN;
    }
    task->parked_on = id;
    return STEP_PARK;
}

// The first turn: the machine, the registrations, and the job rebuilt on it.
// 03 の 4.3改 measures it for a job rather than for a REPL.
static StepResult task_begin(TaskModule *module, Task *task)
{
    task->machine = lhat_machine_new_with_size(TASK_FRAMES, TASK_SLOTS);
    if (task->machine == NULL ||
        !lhat_program_install(module->program, task->machine)) {
        task->status = LHAT_RUN_OUT_OF_MEMORY;
        task->move = MOVE_NONE;
        return STEP_DONE;
    }
    lhat_machine_set_budget(task->machine, TASK_SLICE);

    LhatValue job = lhat_nil();
    if (!lhat_uncarry(task->machine, task->job, &job)) {
        task->status = LHAT_RUN_OUT_OF_MEMORY;
        task->move = MOVE_NONE;
        return STEP_DONE;
    }

    // A closure is called with whatever was written after it; a coroutine
    // carried in (05 の 8.8改3) is already the thing to drive.
    if (!lhat_is_object_kind(job, LHAT_OBJECT_SUBROUTINE)) {
        task->driving = job;
        task->driving_set = true;
        task->ran = lhat_machine_resume(task->machine, job, NULL, 0);
        return after_turn(module, task);
    }

    LhatValue *arguments =
        task->argument_count > 0
            ? (LhatValue *)lhat_calloc(task->argument_count, sizeof *arguments)
            : NULL;
    if (task->argument_count > 0 && arguments == NULL) {
        task->status = LHAT_RUN_OUT_OF_MEMORY;
        task->move = MOVE_NONE;
        return STEP_DONE;
    }
    for (size_t i = 0; i < task->argument_count; i++) {
        if (!lhat_uncarry(task->machine, task->arguments[i], &arguments[i])) {
            lhat_free(arguments);
            task->status = LHAT_RUN_OUT_OF_MEMORY;
            task->move = MOVE_NONE;
            return STEP_DONE;
        }
    }
    task->ran = lhat_machine_call(task->machine, job, arguments,
                                  task->argument_count);
    lhat_free(arguments);
    return after_turn(module, task);
}

// One turn of one task -- exactly one call into the VM, and then the filing.
static StepResult task_turn(TaskModule *module, Task *task)
{
    switch (task->move) {
    case MOVE_START:
        return task_begin(module, task);
    case MOVE_RESUME:
        task->ran = lhat_machine_resume(task->machine, task->driving, NULL, 0);
        return after_turn(module, task);
    case MOVE_CONTINUE:
        task->ran = lhat_machine_continue(task->machine);
        return after_turn(module, task);
    case MOVE_NONE:
    default:
        return STEP_DONE;
    }
}

// The end: the answer carried off the machine before the machine goes, since
// the answer is one of its objects.
static void task_conclude(Task *task)
{
    if (task->status == LHAT_RUN_OK && task->machine != NULL &&
        !lhat_carry(task->ran.value, &task->result, NULL)) {
        task->status = LHAT_RUN_TYPE_ERROR;
    }
    if (task->machine != NULL) {
        lhat_machine_dispose(task->machine);
        task->machine = NULL;
    }
}

// ---------------------------------------------------------------------------
// The worker
// ---------------------------------------------------------------------------

// `module->lock` is held. Every parked task whose wait has come ready moves
// to the run queue. Nothing signals readiness -- a timer's clock reaching
// its deadline is not an event and a push comes from a thread that knows
// nothing of this pool -- so this is a look, and the worker below is what
// decides how often to take it.
static void move_ready(TaskModule *module)
{
    void *waits = lhatstdlib_async_waits(module->program);
    size_t kept = 0;
    for (size_t i = 0; i < module->parked_count; i++) {
        Task *task = module->parked[i];
        // -1 as well as 1: a wait that is no longer there is not one to go
        // on holding a task for.
        if (waits != NULL &&
            lhatstdlib_async_take(waits, task->machine, task->parked_on) == 0) {
            module->parked[kept++] = task;
            continue;
        }
        task->parked_on = 0;
        if (!queue_push(module, task)) {
            module->parked[kept++] = task;  // no room; look again next time
        }
    }
    module->parked_count = kept;
}

// `module->lock` is held.
static bool park(TaskModule *module, Task *task)
{
    if (module->parked_count == module->parked_capacity) {
        size_t wanted =
            module->parked_capacity > 0 ? module->parked_capacity * 2 : 8;
        Task **bigger = (Task **)lhat_calloc(wanted, sizeof *bigger);
        if (bigger == NULL) {
            return false;
        }
        for (size_t i = 0; i < module->parked_count; i++) {
            bigger[i] = module->parked[i];
        }
        lhat_free(module->parked);
        module->parked = bigger;
        module->parked_capacity = wanted;
    }
    module->parked[module->parked_count++] = task;
    return true;
}

// How long a worker with nothing runnable but something parked sleeps before
// looking again. Short, because a push from another thread is not signalled
// -- the same reading std.async's own wait makes.
#define TASK_LOOK_MS 2

// The next task to run, or NULL when the pool is done. Takes and releases
// the lock.
static Task *take_task(TaskModule *module)
{
    lhat_mutex_lock(&module->lock);
    for (;;) {
        if (!module->running) {
            break;
        }
        // BEFORE taking work, and on the clock rather than only when there
        // is nothing to run. A job that neither waits nor ends is runnable
        // for ever, so "look when the queue empties" would never look at
        // all while one of those is in the pool -- and every parked task
        // would wait on it. Rate-limited because the look asks std.async
        // about every parked task, and this loop turns as fast as a slice.
        if (module->parked_count > 0) {
            int64_t now = lhat_now_ms();
            if (now - module->looked_at >= TASK_LOOK_MS) {
                module->looked_at = now;
                move_ready(module);
            }
        }
        Task *task = queue_pop(module);
        if (task != NULL) {
            lhat_mutex_unlock(&module->lock);
            return task;
        }
        if (module->parked_count > 0) {
            // Nothing to run and nothing will say when there is: a timer's
            // deadline is not an event and a push comes from a thread that
            // knows nothing of this pool.
            lhat_mutex_unlock(&module->lock);
            lhat_thread_sleep(TASK_LOOK_MS);
            lhat_mutex_lock(&module->lock);
            continue;
        }
        lhat_condition_wait(&module->work, &module->lock);
    }
    lhat_mutex_unlock(&module->lock);
    return NULL;
}

// Puts a task back where its turn said it belongs. False when the pool is
// going or there is no room, in which case the caller ends it where it
// stands.
static bool file_task(TaskModule *module, Task *task, StepResult step)
{
    lhat_mutex_lock(&module->lock);
    bool filed = module->running &&
                 (step == STEP_PARK ? park(module, task)
                                    : queue_push(module, task));
    lhat_mutex_unlock(&module->lock);
    return filed;
}

static int worker_main(void *raw)
{
    TaskModule *module = (TaskModule *)raw;

    for (;;) {
        Task *task = take_task(module);
        if (task == NULL) {
            break;  // told to stop
        }

        StepResult step = task_turn(module, task);
        if (step != STEP_DONE && file_task(module, task, step)) {
            continue;  // someone will pick it up again, here or elsewhere
        }
        // Done, or the pool went out from under it. Either way this is where
        // it ends: the answer comes off the machine and the machine goes.
        if (step != STEP_DONE) {
            task->status = LHAT_RUN_SUSPENDED;  // left standing
        }
        task_conclude(task);
        task_finished(task);
        task_let_go(task, NULL);  // the queue's hold
    }
    return 0;
}

// ---------------------------------------------------------------------------
// What L^ calls
// ---------------------------------------------------------------------------

static LhatValue fail_with(LhatMachine *machine, const LhatErrorKind *kind,
                           const char *message)
{
    LhatValue error = lhat_nil();
    return lhat_machine_make_error(machine, kind, message, lhat_nil(), &error)
               ? error
               : lhat_nil();
}

static Task *self_of(const TaskModule *module, LhatValue value)
{
    return (Task *)lhat_hostdata_pointer(value, module->tag);
}

static void stop_pool(TaskModule *module)
{
    lhat_mutex_lock(&module->lock);
    if (!module->running) {
        lhat_mutex_unlock(&module->lock);
        return;
    }
    module->running = false;
    // What nobody took is dropped, and whoever holds a handle is told it
    // ended rather than left waiting on a worker that has gone.
    Task *dropped = NULL;
    while ((dropped = queue_pop(module)) != NULL) {
        dropped->status = LHAT_RUN_TYPE_ERROR;
        lhat_mutex_unlock(&module->lock);
        task_finished(dropped);
        task_let_go(dropped, NULL);
        lhat_mutex_lock(&module->lock);
    }
    // And the ones parked on a wait that will never be looked at again.
    // Their machines have frames standing; task_free throws each whole.
    while (module->parked_count > 0) {
        dropped = module->parked[--module->parked_count];
        dropped->status = LHAT_RUN_SUSPENDED;
        lhat_mutex_unlock(&module->lock);
        task_finished(dropped);
        task_let_go(dropped, NULL);
        lhat_mutex_lock(&module->lock);
    }
    lhat_condition_broadcast(&module->work);
    LhatThread *threads = module->threads;
    size_t count = module->worker_count;
    module->threads = NULL;
    module->worker_count = 0;
    lhat_mutex_unlock(&module->lock);

    for (size_t i = 0; i < count; i++) {
        lhat_thread_join(&threads[i]);
    }
    lhat_free(threads);
}

static void task_start(LhatMachine *machine, void *context,
                       const LhatValue *arguments, size_t count,
                       LhatValue *answers, int *answer_count)
{
    TaskModule *module = (TaskModule *)context;
    stop_pool(module);  // a second start is a fresh pool

    int wanted = lhat_cpu_count();
    if (count >= 1) {
        double said = lhat_is_integer(arguments[0])
                          ? (double)lhat_as_integer(arguments[0])
                          : lhat_as_real(arguments[0]);
        // Nothing sensible asked for is one worker: a pool of none would
        // take work and never run it.
        wanted = !(said > 1.0) ? 1 : (said > 1024.0 ? 1024 : (int)said);
    }

    LhatThread *threads =
        (LhatThread *)lhat_calloc((size_t)wanted, sizeof *threads);
    if (threads == NULL) {
        answers[0] = fail_with(machine, module->out_of_memory,
                               "the pool could not be made");
        *answer_count = 1;
        return;
    }
    lhat_mutex_lock(&module->lock);
    module->running = true;
    module->threads = threads;
    module->worker_count = 0;
    lhat_mutex_unlock(&module->lock);

    for (int i = 0; i < wanted; i++) {
        if (!lhat_thread_start(&threads[i], worker_main, module)) {
            break;  // as many as the system would give
        }
        lhat_mutex_lock(&module->lock);
        module->worker_count++;
        lhat_mutex_unlock(&module->lock);
    }

    lhat_mutex_lock(&module->lock);
    size_t started = module->worker_count;
    lhat_mutex_unlock(&module->lock);
    if (started == 0) {
        stop_pool(module);
        answers[0] = fail_with(machine, module->out_of_memory,
                               "no worker could be started");
    } else {
        answers[0] = lhat_integer((int64_t)started);
    }
    *answer_count = 1;
}

static void task_stop(LhatMachine *machine, void *context,
                      const LhatValue *arguments, size_t count,
                      LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)arguments;
    (void)count;
    (void)answers;
    (void)answer_count;
    stop_pool((TaskModule *)context);
}

static void task_workers(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count,
                         LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)arguments;
    (void)count;
    TaskModule *module = (TaskModule *)context;
    lhat_mutex_lock(&module->lock);
    int64_t running = module->running ? (int64_t)module->worker_count : 0;
    lhat_mutex_unlock(&module->lock);
    answers[0] = lhat_integer(running);
    *answer_count = 1;
}

// Takes the value apart, or answers the error a refusal is.
static bool carry_argument(LhatMachine *machine, const TaskModule *module,
                           LhatValue value, LhatCarried **carried,
                           LhatValue *error)
{
    const char *refused = NULL;
    if (lhat_carry(value, carried, &refused)) {
        return true;
    }
    *carried = NULL;
    *error = fail_with(
        machine,
        refused != NULL && strcmp(refused, "out of memory") == 0
            ? module->out_of_memory
            : module->refused,
        refused != NULL ? refused : "out of memory");
    return false;
}

static void task_async(LhatMachine *machine, void *context,
                       const LhatValue *arguments, size_t count,
                       LhatValue *answers, int *answer_count)
{
    TaskModule *module = (TaskModule *)context;
    *answer_count = 1;

    lhat_mutex_lock(&module->lock);
    bool running = module->running;
    lhat_mutex_unlock(&module->lock);
    if (!running) {
        answers[0] = fail_with(machine, module->not_started,
                               "std.task.start has not been called");
        return;
    }
    // A closure that takes '...' and nothing else, or a job carry accepts on
    // its own (a coroutine that has not started, 05 の 8.8改3).
    if (lhat_is_object_kind(arguments[0], LHAT_OBJECT_SUBROUTINE)) {
        const LhatProto *proto = lhat_closure_proto(arguments[0]);
        if (proto == NULL || lhat_proto_yields(proto) ||
            !lhat_proto_has_variadic(proto) ||
            lhat_proto_parameters(proto) != 1) {
            answers[0] = fail_with(
                machine, module->refused,
                "a closure job takes '...' alone and does not yield; "
                "a yieldable one is handed over as the call of it");
            return;
        }
    }

    Task *task = (Task *)lhat_calloc(1, sizeof *task);
    if (task == NULL) {
        answers[0] = fail_with(machine, module->out_of_memory,
                               "out of memory");
        return;
    }
    lhat_mutex_init(&task->lock);
    lhat_condition_init(&task->done);
    task->module = module;
    task->holds = 1;  // the wrapper answered below

    LhatValue error = lhat_nil();
    if (!carry_argument(machine, module, arguments[0], &task->job, &error)) {
        task_let_go(task, NULL);
        answers[0] = error;
        return;
    }
    if (count > 1) {
        task->arguments =
            (LhatCarried **)lhat_calloc(count - 1, sizeof *task->arguments);
        if (task->arguments == NULL) {
            task_let_go(task, NULL);
            answers[0] = fail_with(machine, module->out_of_memory,
                                   "out of memory");
            return;
        }
        task->argument_count = count - 1;
        for (size_t i = 1; i < count; i++) {
            if (!carry_argument(machine, module, arguments[i],
                                &task->arguments[i - 1], &error)) {
                task_let_go(task, NULL);
                answers[0] = error;
                return;
            }
        }
    }

    LhatValue out = lhat_nil();
    if (!lhat_machine_make_hostdata(machine, module->tag, task, &out)) {
        task_let_go(task, NULL);
        answers[0] = fail_with(machine, module->out_of_memory,
                               "out of memory");
        return;
    }
    lhat_mutex_lock(&module->lock);
    task_retain(task, NULL);  // the queue's own hold
    bool queued = module->running && queue_push(module, task);
    lhat_mutex_unlock(&module->lock);
    if (!queued) {
        task_let_go(task, NULL);
        answers[0] = fail_with(machine, module->out_of_memory,
                               "the queue could not grow");
        return;
    }
    answers[0] = out;
}

static void task_await(LhatMachine *machine, void *context,
                       const LhatValue *arguments, size_t count,
                       LhatValue *answers, int *answer_count)
{
    const TaskModule *module = (const TaskModule *)context;
    (void)count;
    *answer_count = 1;
    Task *task = self_of(module, arguments[0]);
    if (task == NULL) {
        answers[0] = fail_with(machine, module->not_started,
                               "not a task of this program");
        return;
    }
    lhat_mutex_lock(&task->lock);
    while (!task->finished) {
        lhat_condition_wait(&task->done, &task->lock);
    }
    LhatRunStatus status = task->status;
    LhatCarried *result = task->result;
    lhat_mutex_unlock(&task->lock);

    if (status != LHAT_RUN_OK) {
        char *said = failure_text(task);
        answers[0] = fail_with(
            machine, module->failed,
            said != NULL ? said : lhat_run_status_message(status));
        lhat_free(said);
        return;
    }
    LhatValue out = lhat_nil();
    if (result != NULL && !lhat_uncarry(machine, result, &out)) {
        answers[0] = fail_with(machine, module->out_of_memory,
                               "out of memory");
        return;
    }
    answers[0] = out;
}

static void task_done(LhatMachine *machine, void *context,
                      const LhatValue *arguments, size_t count,
                      LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)count;
    const TaskModule *module = (const TaskModule *)context;
    Task *task = self_of(module, arguments[0]);
    bool finished = false;
    if (task != NULL) {
        lhat_mutex_lock(&task->lock);
        finished = task->finished;
        lhat_mutex_unlock(&task->lock);
    }
    answers[0] = lhat_bool(finished);
    *answer_count = 1;
}

static void task_failed(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count,
                        LhatValue *answers, int *answer_count)
{
    (void)count;
    const TaskModule *module = (const TaskModule *)context;
    Task *task = self_of(module, arguments[0]);
    LhatValue out = lhat_nil();
    if (task != NULL) {
        lhat_mutex_lock(&task->lock);
        bool finished = task->finished;
        lhat_mutex_unlock(&task->lock);
        if (finished) {
            char *said = failure_text(task);
            if (said != NULL &&
                !lhat_machine_make_string(machine, said, strlen(said), &out)) {
                out = lhat_nil();
            }
            lhat_free(said);
        }
    }
    answers[0] = out;
    *answer_count = 1;
}

// 15.14改: the same push std.thread's handle answers with -- a scheduler
// parks on the wait instead of asking done() over and over.
static void task_awaitable(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count,
                           LhatValue *answers, int *answer_count)
{
    (void)count;
    const TaskModule *module = (const TaskModule *)context;
    Task *task = self_of(module, arguments[0]);
    int64_t id = 0;
    if (task != NULL) {
        lhat_mutex_lock(&task->lock);
        if (task->await_id == 0) {
            task->waits = lhatstdlib_async_waits(module->program);
            task->await_id =
                lhatstdlib_async_external(task->waits, machine);
        }
        id = task->await_id;
        bool already = task->finished;
        void *waits = task->waits;
        lhat_mutex_unlock(&task->lock);
        if (already && id != 0) {
            lhatstdlib_async_complete(waits, id);
        }
    }
    answers[0] = lhat_integer(id);
    *answer_count = 1;
}

static void task_dispose(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count,
                         LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)count;
    (void)answers;
    (void)answer_count;
    // Registered with the tag as its context, not the module: the tag is
    // the process's and the module a program's (stdlib/thread.c says why).
    const LhatHostDataTag *tag = (const LhatHostDataTag *)context;
    Task *task = (Task *)lhat_hostdata_pointer(arguments[0], tag);
    if (task != NULL) {
        task_let_go(task, NULL);
    }
}

void lhatstdlib_task_stop(LhatProgram *program)
{
    TaskModule *module = (TaskModule *)lhat_lookup_host_context(
        program, "std.task", NULL, "async");
    if (module != NULL) {
        stop_pool(module);
    }
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

// The pool goes before the program's bodies do: a worker is reading protos
// the program owns. lhat_program_on_dispose runs while everything of the
// program is still standing, which is what makes this a safe backstop.
static void dispose_module(void *raw)
{
    TaskModule *module = (TaskModule *)raw;
    stop_pool(module);
    lhat_free(module->queue);
    lhat_condition_destroy(&module->work);
    lhat_mutex_destroy(&module->lock);
    lhat_free(module);
}

bool lhatstdlib_task_register(LhatProgram *program)
{
    if (!lhatstdlib_error_register(program)) {
        return false;
    }
    TaskModule *module = (TaskModule *)lhat_calloc(1, sizeof *module);
    if (module == NULL) {
        return false;
    }
    lhat_mutex_init(&module->lock);
    lhat_condition_init(&module->work);
    if (!lhat_program_on_dispose(program, dispose_module, module)) {
        dispose_module(module);
        return false;
    }
    module->program = program;
    module->out_of_memory = lhatstdlib_error_lookup(program, "OutOfMemory");

    static const char *const variants[] = {"NotStarted", "Refused", "Failed"};
    const LhatErrorKind *kinds[3];
    if (!lhat_register_error_kind(program, "std.task", "TaskError", variants,
                                  3, NULL, kinds)) {
        return false;
    }
    module->not_started = kinds[0];
    module->refused = kinds[1];
    module->failed = kinds[2];

    module->tag = lhat_register_hostdata_type(program, "std.task", "Task");
    if (module->tag == NULL) {
        return false;
    }

#define TASK_ERRORS \
    "|std.task.TaskError.NotStarted|std.task.TaskError.Refused" \
    "|std.task.TaskError.Failed|std.error.OutOfMemory;"

    return lhat_register_func(program, "std.task", "start",
                              "p^ -> number^|std.error.OutOfMemory;",
                              task_start, module) &&
           // 14.12: the two shapes of one name, the way a host writes an
           // optional argument.
           lhat_register_func(program, "std.task", "start",
                              "p^number^ -> number^|std.error.OutOfMemory;",
                              task_start, module) &&
           lhat_register_func(program, "std.task", "stop", "p^;", task_stop,
                              module) &&
           lhat_register_func(program, "std.task", "workers",
                              "f^ -> number^;", task_workers, module) &&
           // What a job answers is what carry carries, so any^ and the
           // caller narrows -- the same reading std.thread's join takes.
           lhat_register_func(program, "std.task", "async",
                              "p^any^, ...:any^ -> std.task.Task" TASK_ERRORS,
                              task_async, module) &&
           lhat_register_func(program, "std.task", "await",
                              "p^std.task.Task -> any^" TASK_ERRORS,
                              task_await, module) &&
           lhat_register_member(program, "std.task", "Task", "done",
                                "f^self^ -> bool^;", task_done, module) &&
           lhat_register_member(program, "std.task", "Task", "failed",
                                "f^self^ -> string^|nil^;", task_failed,
                                module) &&
           lhat_register_member(program, "std.task", "Task", "awaitable",
                                "f^self^ -> number^;", task_awaitable,
                                module) &&
           lhat_register_member(program, "std.task", "Task", "dispose",
                                "p^self^;", task_dispose,
                                (void *)module->tag) &&
           // 8.8改2: a Task crosses machines, so a job may be handed one.
           lhat_register_hostdata_shared(program, "std.task", "Task",
                                         task_retain, task_let_go, NULL);

#undef TASK_ERRORS
}
