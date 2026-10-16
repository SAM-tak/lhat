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
// it with try^/catch^, or narrow with fits^ against std.random.Random
// (05 の 8.8's hostdata path). See error.h for why OutOfMemory lives in
// std.error rather than a std.random.RandomError of its own.

#include "error.h"
#include "random.h"

#include <math.h>
#include <stdint.h>
#include <time.h>

// What lhatstdlib_random_register made, threaded through as every
// registration's `context` (05 § 8.7) -- see stdlib/io.c's IoModule
// comment for why this isn't a file-scope static instead.
typedef struct {
    const LhatErrorKind *out_of_memory;
    const LhatErrorKind *invalid_state;
    const LhatErrorKind *invalid_argument;
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
static uint64_t seeded_state(LhatInteger seed)
{
    uint64_t mixed = (uint64_t)seed ^ 0x9E3779B97F4A7C15ULL;
    return mixed != 0 ? mixed : 0x2545F4914F6CDD1DULL;
}

static LhatInteger arg_as_integer(LhatValue value)
{
    return lhat_is_integer(value) ? lhat_as_integer(value)
                                  : (LhatInteger)lhat_as_real(value);
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
static void random_new(LhatMachine *machine, void *context,
                       const LhatValue *arguments, size_t count,
                       LhatValue *answers, int *answer_count)
{
    (void)count;
    const RandomModule *module = (const RandomModule *)context;

    Random *r = (Random *)lhat_alloc(sizeof *r);
    if (r == NULL) {
        answers[0] = fail_with(machine, module->out_of_memory, "out of memory");
        *answer_count = 1;
        return;
    }
    LhatInteger seed = arg_as_integer(arguments[0]);
    r->state = seeded_state(seed != 0 ? seed : (LhatInteger)time(NULL));

    LhatValue out = lhat_nil();
    if (!lhat_machine_make_hostdata(machine, module->tag, r, &out)) {
        lhat_free(r);
        answers[0] = fail_with(machine, module->out_of_memory, "out of memory");
        *answer_count = 1;
        return;
    }
    answers[0] = out;
    *answer_count = 1;
}

static void random_reseed(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count,
                          LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)count;
    (void)answers;
    (void)answer_count;
    const RandomModule *module = (const RandomModule *)context;
    Random *r = (Random *)lhat_hostdata_pointer(arguments[0], module->tag);
    if (r != NULL) {
        r->state = seeded_state(arg_as_integer(arguments[1]));
    }
}

// Encode all 64 state bits as a signed integer without a real conversion.
static void random_save(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count,
                        LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)count;
    const RandomModule *module = (const RandomModule *)context;
    Random *r = (Random *)lhat_hostdata_pointer(arguments[0], module->tag);
    uint64_t state = r != NULL ? r->state : 0;
    LhatInteger saved = state <= INT64_MAX ? (LhatInteger)state
                                         : -1 - (LhatInteger)(~state);
    answers[0] = lhat_integer(saved);
    *answer_count = 1;
}

static void random_load(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count,
                        LhatValue *answers, int *answer_count)
{
    (void)count;
    const RandomModule *module = (const RandomModule *)context;
    Random *r = (Random *)lhat_hostdata_pointer(arguments[0], module->tag);
    LhatInteger state;
    answers[0] = lhat_nil();
    *answer_count = 1;
    if (!lhat_number_as_whole(arguments[1], &state) || state == 0) {
        answers[0] = fail_with(machine, module->invalid_state,
                               "random state must be a nonzero 64-bit integer");
        return;
    }
    if (r != NULL) {
        r->state = (uint64_t)state;
    }
}

// [0, 1). The top 53 bits are what a double's mantissa can hold exactly.
static LhatReal next_unit(Random *r)
{
    return (LhatReal)(next_raw(r) >> 11) / (LhatReal)(1ULL << 53);
}

static void random_next(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count,
                        LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)count;
    const RandomModule *module = (const RandomModule *)context;
    Random *r = (Random *)lhat_hostdata_pointer(arguments[0], module->tag);
    if (r == NULL) {
        answers[0] = lhat_real(0.0);
        *answer_count = 1;
        return;
    }
    answers[0] = lhat_real(next_unit(r));
    *answer_count = 1;
}

// An integer in [lo, hi], both ends inclusive -- 04 § 11.3's "no such thing
// as out of range" does not apply here, this is an ordinary argument.
static void random_rangeint(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count,
                         LhatValue *answers, int *answer_count)
{
    (void)count;
    const RandomModule *module = (const RandomModule *)context;
    Random *r = (Random *)lhat_hostdata_pointer(arguments[0], module->tag);
    LhatInteger lo, hi;
    *answer_count = 1;
    if (!lhat_number_as_whole(arguments[1], &lo) ||
        !lhat_number_as_whole(arguments[2], &hi) || hi < lo) {
        answers[0] = fail_with(machine, module->invalid_argument,
                               "rangeint requires ordered 64-bit integer bounds");
        return;
    }
    if (r == NULL || hi <= lo) {
        answers[0] = lhat_integer(lo);
        *answer_count = 1;
        return;
    }
    // Unsigned arithmetic also handles spans crossing zero without overflow.
    uint64_t span = (uint64_t)hi - (uint64_t)lo + 1;
    uint64_t bits = next_raw(r);
    if (span != 0) {
        // Discard the short remainder so every residue has the same count.
        uint64_t threshold = (UINT64_C(0) - span) % span;
        while (bits < threshold) {
            bits = next_raw(r);
        }
        bits %= span;
    }
    // A zero span denotes the full 2^64 range and needs no reduction.
    uint64_t result = (uint64_t)lo + bits;
    answers[0] = lhat_integer(result <= INT64_MAX ? (LhatInteger)result
                                               : -1 - (LhatInteger)(~result));
    *answer_count = 1;
}

static void random_uniform(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count,
                           LhatValue *answers, int *answer_count)
{
    (void)count;
    const RandomModule *module = (const RandomModule *)context;
    Random *r = (Random *)lhat_hostdata_pointer(arguments[0], module->tag);
    LhatReal lo = lhat_number_as_real(arguments[1]);
    LhatReal hi = lhat_number_as_real(arguments[2]);
    *answer_count = 1;
    if (!isfinite(lo) || !isfinite(hi) || hi < lo) {
        answers[0] = fail_with(machine, module->invalid_argument,
                               "uniform requires ordered finite bounds");
        return;
    }
    if (lo == hi || r == NULL) {
        answers[0] = lhat_real(lo);
        return;
    }
    LhatReal u = next_unit(r);
    // Opposite-sign bounds can overflow hi - lo even when both are finite.
    LhatReal value = lo <= 0 && hi >= 0 ? (1.0 - u) * lo + u * hi
                                       : lo + u * (hi - lo);
    // Interpolation may round up to hi; enforce the half-open contract.
    if (value >= hi) {
        value = nextafter(hi, lo);
    }
    answers[0] = lhat_real(value < lo ? lo : value);
}

static void random_normal(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count,
                          LhatValue *answers, int *answer_count)
{
    (void)count;
    const RandomModule *module = (const RandomModule *)context;
    Random *r = (Random *)lhat_hostdata_pointer(arguments[0], module->tag);
    LhatReal mean = lhat_number_as_real(arguments[1]);
    LhatReal deviation = lhat_number_as_real(arguments[2]);
    *answer_count = 1;
    if (!isfinite(mean) || !isfinite(deviation) || deviation < 0) {
        answers[0] = fail_with(machine, module->invalid_argument,
                               "normal requires a finite mean and a finite nonnegative standard deviation");
        return;
    }
    if (deviation == 0 || r == NULL) {
        answers[0] = arguments[1];
        return;
    }
    // Box-Muller: (0, 1] avoids log(0). Discard the second normal sample
    // so the generator's single integer remains its entire saved state.
    LhatReal radius = sqrt(-2.0 * log(1.0 - next_unit(r)));
    LhatReal angle = 6.283185307179586476925286766559 * next_unit(r);
    answers[0] = lhat_real(mean + deviation * (radius * cos(angle)));
}

// 05 § 8.8: registering this is what makes a Random the host's to hand
// over and L^'s to give back.
static void random_dispose(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count,
                           LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)count;
    (void)answers;
    (void)answer_count;
    const RandomModule *module = (const RandomModule *)context;
    lhat_free(lhat_hostdata_pointer(arguments[0], module->tag));
}

bool lhatstdlib_random_register(LhatProgram *program)
{
    // 05 § 8.7: registration precedes checking -- ensure std.error.OutOfMemory
    // exists before this module uses it. lhatstdlib_error_register is
    // idempotent, so another stdlib module may already have called it.
    if (!lhatstdlib_error_register(program)) {
        return false;
    }

    // 05 § 8.7: every field of this is an identity, and an identity
    // belongs to the process rather than to a program -- one declaration,
    // one tag and one error kind, however many programs declare them. So
    // one of these serves them all, and a second registration writes the
    // same answers back into it.
    static RandomModule shared;
    RandomModule *module = &shared;
    module->out_of_memory = lhatstdlib_error_lookup(program, "OutOfMemory");
    static const char *const variants[] = {"InvalidState", "InvalidArgument"};
    if (!lhat_register_error_kind(program, "std.random", "Error", variants, 2,
                                  NULL, NULL)) {
        return false;
    }
    module->invalid_state =
        lhat_lookup_error_kind(program, "std.random", "Error", "InvalidState");
    module->invalid_argument =
        lhat_lookup_error_kind(program, "std.random", "Error", "InvalidArgument");

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
           lhat_register_member(program, "std.random", "Random", "save",
                                "f^self^ -> number^;", random_save, module) &&
           lhat_register_member(program, "std.random", "Random", "load",
                                "p^self^, number^ -> nil^|std.random.Error.InvalidState;",
                                random_load, module) &&
           lhat_register_member(program, "std.random", "Random", "next",
                                "f^self^ -> number^;", random_next, module) &&
           lhat_register_member(program, "std.random", "Random", "rangeint",
                                "f^self^, number^, number^ -> number^|std.random.Error.InvalidArgument;",
                                random_rangeint, module) &&
           lhat_register_member(program, "std.random", "Random", "uniform",
                                "f^self^, number^, number^ -> number^|std.random.Error.InvalidArgument;",
                                random_uniform, module) &&
           lhat_register_member(program, "std.random", "Random", "normal",
                                "f^self^, number^, number^ -> number^|std.random.Error.InvalidArgument;",
                                random_normal, module) &&
           lhat_register_member(program, "std.random", "Random", "dispose",
                                "p^self^;", random_dispose, module);
}
