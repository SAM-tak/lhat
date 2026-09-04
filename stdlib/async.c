// L^ (lhat) -- sample standard library: std.async.
//
// A table of things a task may be waiting for, and the two questions a
// scheduler asks about it: what is ready, and how long until the next one
// could be. See async.h for the contract a host is held to, and 02 の 15.14
// for why the scheduler itself is L^ rather than any of this.
//
// Two kinds of wait live in the one table. A deadline comes due by itself,
// and the clock decides when; an external one is due when somebody says so,
// which is what lhatstdlib_async_complete is. Telling them apart matters
// only for `next`: an external wait has no time to wait until.
//
// The table is locked. A deadline is only ever touched by the machine that
// armed it, but a completion may arrive from any thread -- a worker that has
// finished, a host's signal handler, an engine's loader -- and that is the
// whole point of the external kind.

#include "async.h"

#include "port/thread.h"

#include <stdint.h>

typedef struct {
    int64_t id;
    int64_t due_ms;  // when the clock will make it ready
    bool timed;      // false for an external one: only a push makes it ready
    bool ready;      // pushed, and not yet handed back
} Waiting;

typedef struct {
    LhatMutex lock;
    Waiting *waits;
    size_t count;
    size_t capacity;
    int64_t next_id;
} AsyncModule;

// 05 の 8.9: seconds are what a caller has in mind, and the clock counts
// milliseconds -- the same conversion std.thread.sleep makes, at the same
// boundary. Nothing to wait for is not an error: a deadline in the past is
// one that is already due.
static int64_t milliseconds_of(LhatValue value)
{
    double seconds = lhat_is_integer(value) ? (double)lhat_as_integer(value)
                                            : lhat_as_real(value);
    double ms = seconds * 1000.0;
    if (!(ms > 0.0)) {  // answers for a NaN as well
        return 0;
    }
    return ms >= (double)INT64_MAX ? INT64_MAX : (int64_t)ms;
}

// The lock is held. Answers NULL when the table could not grow.
static Waiting *add_wait(AsyncModule *module, bool timed, int64_t due_ms)
{
    if (module->count == module->capacity) {
        size_t grown = module->capacity == 0 ? 8 : module->capacity * 2;
        Waiting *moved =
            (Waiting *)lhat_realloc(module->waits, grown * sizeof *moved);
        if (moved == NULL) {
            return NULL;
        }
        module->waits = moved;
        module->capacity = grown;
    }
    Waiting *added = &module->waits[module->count++];
    added->id = module->next_id++;
    added->due_ms = due_ms;
    added->timed = timed;
    added->ready = false;
    return added;
}

// The lock is held. The first wait that is ready now -- pushed, or a deadline
// the clock has passed -- taken out of the table.
static bool take_ready(AsyncModule *module, int64_t now, int64_t *id)
{
    for (size_t i = 0; i < module->count; i++) {
        Waiting *wait = &module->waits[i];
        if (wait->ready || (wait->timed && wait->due_ms <= now)) {
            *id = wait->id;
            module->waits[i] = module->waits[--module->count];
            return true;
        }
    }
    return false;
}

// The lock is held. Milliseconds until the earliest deadline, or -1 when
// nothing is waiting on the clock. An external wait answers nothing here:
// there is no time at which it becomes ready.
static int64_t until_next(const AsyncModule *module, int64_t now)
{
    int64_t soonest = -1;
    for (size_t i = 0; i < module->count; i++) {
        const Waiting *wait = &module->waits[i];
        if (!wait->timed) {
            continue;
        }
        int64_t left = wait->due_ms - now;
        if (left < 0) {
            left = 0;
        }
        if (soonest < 0 || left < soonest) {
            soonest = left;
        }
    }
    return soonest;
}

static void async_timer(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count,
                        LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)count;
    AsyncModule *module = (AsyncModule *)context;
    lhat_mutex_lock(&module->lock);
    Waiting *armed =
        add_wait(module, true, lhat_now_ms() + milliseconds_of(arguments[0]));
    int64_t id = armed != NULL ? armed->id : 0;
    lhat_mutex_unlock(&module->lock);
    // 0 is no wait at all: ids start at 1, so a table that could not grow
    // answers something the L^ side already reads as "not waiting".
    answers[0] = lhat_integer(id);
    *answer_count = 1;
}

// A wait nothing but a push can end. What the host holds the id for is its
// own business -- a signal, a loader's status, a queue of its own.
static void async_external(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count,
                           LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)arguments;
    (void)count;
    // The same call a library makes on a program's behalf (async.h).
    answers[0] = lhat_integer(lhatstdlib_async_external(context));
    *answer_count = 1;
}

int64_t lhatstdlib_async_external(void *waits)
{
    AsyncModule *module = (AsyncModule *)waits;
    if (module == NULL) {
        return 0;
    }
    lhat_mutex_lock(&module->lock);
    Waiting *armed = add_wait(module, false, 0);
    int64_t id = armed != NULL ? armed->id : 0;
    lhat_mutex_unlock(&module->lock);
    // 0 is no wait at all: ids start at 1, so a table that could not grow
    // answers something the L^ side already reads as "not waiting".
    return id;
}

// Whoever armed a wait may give up on it. An external one nobody will ever
// push would otherwise sit in the table for ever, and `pending` would never
// reach zero -- which is what a scheduler reads to know it is finished.
static void async_drop(LhatMachine *machine, void *context,
                       const LhatValue *arguments, size_t count,
                       LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)count;
    (void)answers;
    (void)answer_count;
    AsyncModule *module = (AsyncModule *)context;
    int64_t id = lhat_is_integer(arguments[0]) ? lhat_as_integer(arguments[0])
                                               : 0;
    lhat_mutex_lock(&module->lock);
    for (size_t i = 0; i < module->count; i++) {
        if (module->waits[i].id == id) {
            module->waits[i] = module->waits[--module->count];
            break;
        }
    }
    lhat_mutex_unlock(&module->lock);
}

// The one call that may sleep, and only for as long as it is given. Zero is
// the shape a host pumping its own loop uses: take what is ready and come
// straight back.
static void async_wait(LhatMachine *machine, void *context,
                       const LhatValue *arguments, size_t count,
                       LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)count;
    AsyncModule *module = (AsyncModule *)context;
    int64_t give_up_at = lhat_now_ms() + milliseconds_of(arguments[0]);

    for (;;) {
        lhat_mutex_lock(&module->lock);
        int64_t now = lhat_now_ms();
        int64_t id = 0;
        if (take_ready(module, now, &id)) {
            lhat_mutex_unlock(&module->lock);
            answers[0] = lhat_integer(id);
            *answer_count = 1;
            return;
        }
        int64_t soonest = until_next(module, now);
        lhat_mutex_unlock(&module->lock);

        if (now >= give_up_at) {
            return;
        }
        // To whichever comes first, and look again -- a wait may have been
        // armed or pushed while this was asleep. A push from another thread
        // is not signalled: the sleep below is short by construction (a
        // caller with something else to watch passes a small patience), and
        // a condition variable here would tie every host to waking it.
        int64_t until = give_up_at;
        if (soonest >= 0 && now + soonest < until) {
            until = now + soonest;
        }
        int64_t nap = until - now;
        if (nap > 20) {
            nap = 20;  // 20ms is the longest anything waits to notice a push
        }
        lhat_thread_sleep(nap > INT32_MAX ? INT32_MAX : (int)nap);
    }
}

// Seconds until the earliest deadline, or nil^ when nothing is waiting on the
// clock. What a host reads to decide its own sleep -- a game compares it with
// the time left in the frame and mostly throws it away.
static void async_next(LhatMachine *machine, void *context,
                       const LhatValue *arguments, size_t count,
                       LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)arguments;
    (void)count;
    AsyncModule *module = (AsyncModule *)context;
    lhat_mutex_lock(&module->lock);
    int64_t left = until_next(module, lhat_now_ms());
    lhat_mutex_unlock(&module->lock);
    answers[0] = left < 0 ? lhat_nil() : lhat_real((double)left / 1000.0);
    *answer_count = 1;
}

// How many waits are still in the table. A scheduler with no task awake and
// nothing pending has nothing left to do, and this is how it knows.
static void async_pending(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count,
                          LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)arguments;
    (void)count;
    AsyncModule *module = (AsyncModule *)context;
    lhat_mutex_lock(&module->lock);
    int64_t pending = (int64_t)module->count;
    lhat_mutex_unlock(&module->lock);
    answers[0] = lhat_integer(pending);
    *answer_count = 1;
}

// The host's half of an external wait. Any thread may call it; the id is what
// std.async.external answered, and nothing else is a valid one.
bool lhatstdlib_async_complete(void *waits, int64_t id)
{
    AsyncModule *module = (AsyncModule *)waits;
    if (module == NULL) {
        return false;
    }
    bool found = false;
    lhat_mutex_lock(&module->lock);
    for (size_t i = 0; i < module->count; i++) {
        if (module->waits[i].id == id) {
            module->waits[i].ready = true;
            found = true;
            break;
        }
    }
    lhat_mutex_unlock(&module->lock);
    return found;
}

void *lhatstdlib_async_waits(const LhatProgram *program)
{
    // 05 の 8.7: the context every registration of this module was given.
    // Asked of the program rather than kept in a static, for the reason
    // stdlib/io.c gives -- two programs are two tables.
    return lhat_lookup_host_context(program, "std.async", NULL, "external");
}

// 05 の 8.7: what this module leaves with the program. Unlike the other
// modules' this one owns more than itself -- the waits it is holding and the
// lock over them.
static void dispose_async(void *context)
{
    AsyncModule *module = (AsyncModule *)context;
    lhat_mutex_destroy(&module->lock);
    lhat_free(module->waits);
    lhat_free(module);
}

bool lhatstdlib_async_register(LhatProgram *program)
{
    AsyncModule *module = (AsyncModule *)lhat_calloc(1, sizeof *module);
    if (module == NULL) {
        return false;
    }
    lhat_mutex_init(&module->lock);
    if (!lhat_program_on_dispose(program, dispose_async, module)) {
        dispose_async(module);
        return false;
    }
    // 0 stands for no wait on the L^ side, so ids start above it.
    module->next_id = 1;

    return lhat_register_func(program, "std.async", "timer",
                              "f^number^ -> number^;", async_timer, module) &&
           lhat_register_func(program, "std.async", "external",
                              "f^ -> number^;", async_external, module) &&
           lhat_register_func(program, "std.async", "drop", "p^number^;",
                              async_drop, module) &&
           lhat_register_func(program, "std.async", "wait",
                              "f^number^ -> number^|nil^;", async_wait,
                              module) &&
           lhat_register_func(program, "std.async", "next",
                              "f^ -> number^|nil^;", async_next, module) &&
           lhat_register_func(program, "std.async", "pending",
                              "f^ -> number^;", async_pending, module);
}
