// L^ (lhat) -- sample standard library: std.random.
//
// std.random.new(seed) answers a std.random.Random hostdata (05 の 8.8),
// one xorshift64* stream per instance. No module-wide mutable state here,
// so no lock either -- a program sharing one Random across std.thread
// spawns is on its own for keeping next()/range()/reseed() from racing on
// it, the same way it would be on its own for racing a table it shared the
// same way. This file only ever touches one instance's own memory.
//
// xorshift64* (Marsaglia/Vigna): small, dependency-free, good enough for the
// games and tools this is a sample for. Not cryptographic.
//
// std.random.new answers std.random.Random|std.error.OutOfMemory -- read
// it with try^/catch^, or narrow with isa^ against std.random.Random
// (05 の 8.8's hostdata path). See error.h for why OutOfMemory lives in
// std.error rather than a std.random.RandomError of its own.

#include "error.h"
#include "random.h"

#include <stdint.h>
#include <time.h>

// What lhatstdlib_random_register made, threaded through as every
// registration's `context` (05 の 8.7) -- see stdlib/io.c's IoModule
// comment for why this isn't a file-scope static instead.
typedef struct {
    const LhatErrorKind *out_of_memory;
    const LhatHostDataTag *tag;
} RandomModule;

typedef struct {
    uint64_t state;
} Random;

static LhatValue fail_with(LhatMachine *machine, const LhatErrorKind *kind,
                           const char *message)
{
    LhatValue error = lhat_nil();
    return lhat_machine_make_error(machine, kind, message, lhat_nil(), &error)
               ? error
               : lhat_nil();
}

// xorshift64* refuses an all-zero state, so a seed of exactly that value
// (or 0 itself, once XORed with the mixing constant below) is nudged aside.
static uint64_t seeded_state(int64_t seed)
{
    uint64_t mixed = (uint64_t)seed ^ 0x9E3779B97F4A7C15ULL;
    return mixed != 0 ? mixed : 0x2545F4914F6CDD1DULL;
}

static int64_t arg_as_integer(LhatValue value)
{
    return lhat_is_integer(value) ? lhat_as_integer(value)
                                  : (int64_t)lhat_as_real(value);
}

static uint64_t next_raw(Random *r)
{
    r->state ^= r->state >> 12;
    r->state ^= r->state << 25;
    r->state ^= r->state >> 27;
    return r->state * 0x2545F4914F6CDD1DULL;
}

// seed == 0 asks for one seeded from the clock, so a caller that does not
// care still gets a different sequence each run without writing time(NULL)
// itself -- std/ has no clock module of its own to reach for that.
static LhatValue random_new(LhatMachine *machine, void *context,
                            const LhatValue *arguments, size_t count)
{
    (void)count;
    const RandomModule *module = (const RandomModule *)context;

    Random *r = (Random *)lhat_alloc(sizeof *r);
    if (r == NULL) {
        return fail_with(machine, module->out_of_memory, "out of memory");
    }
    int64_t seed = arg_as_integer(arguments[0]);
    r->state = seeded_state(seed != 0 ? seed : (int64_t)time(NULL));

    LhatValue out = lhat_nil();
    if (!lhat_machine_make_hostdata(machine, module->tag, r, &out)) {
        lhat_free(r);
        return fail_with(machine, module->out_of_memory, "out of memory");
    }
    return out;
}

static LhatValue random_reseed(LhatMachine *machine, void *context,
                               const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)count;
    const RandomModule *module = (const RandomModule *)context;
    Random *r = (Random *)lhat_hostdata_pointer(arguments[0], module->tag);
    if (r != NULL) {
        r->state = seeded_state(arg_as_integer(arguments[1]));
    }
    return lhat_nil();
}

// [0, 1). The top 53 bits are what a double's mantissa can hold exactly.
static LhatValue random_next(LhatMachine *machine, void *context,
                             const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)count;
    const RandomModule *module = (const RandomModule *)context;
    Random *r = (Random *)lhat_hostdata_pointer(arguments[0], module->tag);
    if (r == NULL) {
        return lhat_real(0.0);
    }
    uint64_t bits = next_raw(r) >> 11;
    return lhat_real((double)bits / (double)(1ULL << 53));
}

// An integer in [lo, hi], both ends inclusive -- 04 の 11.3's "no such thing
// as out of range" does not apply here, this is an ordinary argument.
static LhatValue random_range(LhatMachine *machine, void *context,
                              const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)count;
    const RandomModule *module = (const RandomModule *)context;
    Random *r = (Random *)lhat_hostdata_pointer(arguments[0], module->tag);
    double lo = lhat_number_as_real(arguments[1]);
    double hi = lhat_number_as_real(arguments[2]);
    if (r == NULL || hi <= lo) {
        return lhat_integer((int64_t)lo);
    }
    uint64_t span = (uint64_t)(hi - lo) + 1;
    uint64_t offset = next_raw(r) % span;
    return lhat_integer((int64_t)lo + (int64_t)offset);
}

// 05 の 8.8: registering this is what makes a Random the host's to hand
// over and L^'s to give back.
static LhatValue random_dispose(LhatMachine *machine, void *context,
                                const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)count;
    const RandomModule *module = (const RandomModule *)context;
    lhat_free(lhat_hostdata_pointer(arguments[0], module->tag));
    return lhat_nil();
}

bool lhatstdlib_random_register(LhatProgram *program)
{
    // 05 の 8.7: 登録は検査より前 -- std.error.OutOfMemory を使う側(この
    // モジュール)より前に確実に存在させる。lhatstdlib_error_register は
    // 冪等なので、他の stdlib モジュールが先に呼んでいても構わない。
    if (!lhatstdlib_error_register(program)) {
        return false;
    }

    RandomModule *module = (RandomModule *)lhat_calloc(1, sizeof *module);
    if (module == NULL) {
        return false;
    }
    // 05 の 8.7: the program gives this back when it goes -- a register
    // that answers bool hands its caller no handle to free it with.
    if (!lhat_program_on_dispose(program, lhat_free, module)) {
        lhat_free(module);
        return false;
    }
    module->out_of_memory = lhatstdlib_error_lookup(program, "OutOfMemory");

    module->tag = lhat_register_hostdata_type(program, "std.random", "Random");
    if (module->tag == NULL) {
        return false;
    }

    return lhat_register_func(
               program, "std.random", "new",
               "f^number^ -> std.random.Random|std.error.OutOfMemory;",
               random_new, module) &&
           lhat_register_member(program, "std.random", "Random", "reseed",
                                "p^self^, number^;", random_reseed, module) &&
           lhat_register_member(program, "std.random", "Random", "next",
                                "f^self^ -> number^;", random_next, module) &&
           lhat_register_member(program, "std.random", "Random", "range",
                                "f^self^, number^, number^ -> number^;",
                                random_range, module) &&
           lhat_register_member(program, "std.random", "Random", "dispose",
                                "p^self^;", random_dispose, module);
}
