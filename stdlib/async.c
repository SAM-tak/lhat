// L^ (lhat) -- sample standard library: std.async.
//
// A table of deadlines and a way to be idle until one comes due. See async.h
// for why that is all of it, and 02 の 15.14 for why a scheduler written in
// L^ over these is the shape this is for.
//
// No lock. 05 の 8.9's own reasoning applies here: the table belongs to
// whichever machine calls these, and the one thing this module is for -- a
// single scheduler running one machine's tasks -- touches it from one thread.
// A second machine (std.thread's spawn makes one) that reaches for the same
// registration would be sharing the context every registration shares, and a
// program doing that is on its own the way stdlib/random.c says it is.

#include "async.h"

#include "port/thread.h"

#include <stdint.h>

// A deadline that has been armed and not yet handed back. `id` is what the
// L^ side holds while it waits; 0 is never one, so it can stand for "none".
typedef struct {
    int64_t id;
    int64_t due_ms;
} Deadline;

typedef struct {
    Deadline *deadlines;
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

static LhatValue async_timer(LhatMachine *machine, void *context,
                             const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)count;
    AsyncModule *module = (AsyncModule *)context;
    if (module->count == module->capacity) {
        size_t grown = module->capacity == 0 ? 8 : module->capacity * 2;
        Deadline *moved = (Deadline *)lhat_realloc(
            module->deadlines, grown * sizeof *moved);
        if (moved == NULL) {
            return lhat_nil();
        }
        module->deadlines = moved;
        module->capacity = grown;
    }
    Deadline *armed = &module->deadlines[module->count++];
    armed->id = module->next_id++;
    armed->due_ms = lhat_now_ms() + milliseconds_of(arguments[0]);
    return lhat_integer(armed->id);
}

// The one place a scheduler blocks. Answers the id of a deadline that has
// come due, or nil^ when the caller's own patience ran out first -- a loop
// with something else to watch passes a short one and keeps its turn.
static LhatValue async_wait(LhatMachine *machine, void *context,
                            const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)count;
    AsyncModule *module = (AsyncModule *)context;
    int64_t give_up_at = lhat_now_ms() + milliseconds_of(arguments[0]);

    for (;;) {
        // The earliest of what is armed, since that is the next thing that
        // can happen. Ties go to whichever was found first; nothing here
        // promises an order among deadlines that fall together.
        size_t soonest = module->count;
        for (size_t i = 0; i < module->count; i++) {
            if (soonest == module->count ||
                module->deadlines[i].due_ms < module->deadlines[soonest].due_ms) {
                soonest = i;
            }
        }

        int64_t now = lhat_now_ms();
        if (soonest < module->count && module->deadlines[soonest].due_ms <= now) {
            int64_t id = module->deadlines[soonest].id;
            module->deadlines[soonest] = module->deadlines[--module->count];
            return lhat_integer(id);
        }
        if (now >= give_up_at) {
            return lhat_nil();
        }

        // Sleep to whichever comes first, and look again -- the deadline may
        // have been armed by a task this same turn.
        int64_t until = give_up_at;
        if (soonest < module->count && module->deadlines[soonest].due_ms < until) {
            until = module->deadlines[soonest].due_ms;
        }
        int64_t nap = until - now;
        lhat_thread_sleep(nap > INT32_MAX ? INT32_MAX : (int)nap);
    }
}

// Whether anything is still armed. A scheduler with no task awake and no
// deadline pending has nothing left to wait for, and this is how it knows.
static LhatValue async_pending(LhatMachine *machine, void *context,
                               const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)arguments;
    (void)count;
    const AsyncModule *module = (const AsyncModule *)context;
    return lhat_integer((int64_t)module->count);
}

bool lhatstdlib_async_register(LhatProgram *program)
{
    AsyncModule *module = (AsyncModule *)lhat_calloc(1, sizeof *module);
    if (module == NULL) {
        return false;
    }
    // 0 stands for no deadline on the L^ side, so ids start above it.
    module->next_id = 1;

    return lhat_register_func(program, "std.async", "timer",
                              "f^number^ -> number^;", async_timer, module) &&
           lhat_register_func(program, "std.async", "wait",
                              "f^number^ -> number^|nil^;", async_wait,
                              module) &&
           lhat_register_func(program, "std.async", "pending",
                              "f^ -> number^;", async_pending, module);
}
