// L^ (lhat) -- sample standard library: std.random.
//
// One generator, process-wide -- the simplest thing that could work. A host
// wanting more than one independent stream copies this file and turns
// `state` into a std.io.File-shaped hostdata (05 の 8.8) instead; nothing
// about the registration below has to change to get there.
//
// xorshift64* (Marsaglia/Vigna): small, dependency-free, good enough for the
// games and tools this is a sample for. Not cryptographic.
//
// `state` is one process-wide stream, so std.thread.spawn puts it within
// reach of more than one OS thread at once -- `lock` is what keeps
// next_raw()/random_seed()'s read-modify-write of it from racing.

#include "random.h"

#include <stdint.h>
#include <threads.h>
#include <time.h>

static uint64_t state = 0;
static mtx_t lock;
static once_flag lock_ready = ONCE_FLAG_INIT;

static void init_lock(void)
{
    mtx_init(&lock, mtx_plain);
}

// Seeded from the clock the first time anything asks, so a program that
// never calls seed() still gets a different sequence each run.
static uint64_t next_raw(void)
{
    call_once(&lock_ready, init_lock);
    mtx_lock(&lock);
    if (state == 0) {
        state = (uint64_t)time(NULL) ^ 0x9E3779B97F4A7C15ULL;
        if (state == 0) {
            state = 0x9E3779B97F4A7C15ULL;  // the one seed xorshift64* refuses
        }
    }
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    uint64_t out = state * 0x2545F4914F6CDD1DULL;
    mtx_unlock(&lock);
    return out;
}

static LhatValue random_seed(LhatMachine *machine, void *context,
                             const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    (void)count;
    int64_t n = lhat_is_integer(arguments[0])
                    ? lhat_as_integer(arguments[0])
                    : (int64_t)lhat_as_real(arguments[0]);
    call_once(&lock_ready, init_lock);
    mtx_lock(&lock);
    state = (uint64_t)n ^ 0x9E3779B97F4A7C15ULL;
    if (state == 0) {
        state = 0x9E3779B97F4A7C15ULL;
    }
    mtx_unlock(&lock);
    return lhat_nil();
}

// [0, 1). The top 53 bits are what a double's mantissa can hold exactly.
static LhatValue random_next(LhatMachine *machine, void *context,
                             const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    (void)arguments;
    (void)count;
    uint64_t bits = next_raw() >> 11;
    return lhat_real((double)bits / (double)(1ULL << 53));
}

// An integer in [lo, hi], both ends inclusive -- 04 の 11.3's "no such thing
// as out of range" does not apply here, this is an ordinary argument.
static LhatValue random_range(LhatMachine *machine, void *context,
                              const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    (void)count;
    double lo = lhat_number_as_real(arguments[0]);
    double hi = lhat_number_as_real(arguments[1]);
    if (hi <= lo) {
        return lhat_integer((int64_t)lo);
    }
    uint64_t span = (uint64_t)(hi - lo) + 1;
    uint64_t offset = next_raw() % span;
    return lhat_integer((int64_t)lo + (int64_t)offset);
}

bool lhatstdlib_random_register(LhatProgram *program)
{
    return lhat_register_func(program, "std.random", "seed", "p^number^;",
                              random_seed, NULL) &&
           lhat_register_func(program, "std.random", "next", "f^ -> number^;",
                              random_next, NULL) &&
           lhat_register_func(program, "std.random", "range",
                              "f^number^, number^ -> number^;", random_range,
                              NULL);
}
