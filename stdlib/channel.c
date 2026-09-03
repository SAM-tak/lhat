// L^ (lhat) -- sample standard library: std.channel (see channel.h).
//
// A queue of carried values (carry.h) behind one lock and one condition.
// Every value is taken apart by the pushing machine and put back together by
// the taking one, so what sits in the queue belongs to no machine -- which is
// what lets two of them share a channel at all.
//
// LÖVE's Channel is the shape being followed: push answers a number, supply
// waits for that number to be read, demand waits for a value, clear settles
// every waiting supply. The counters are why that works with one condition
// and no per-waiter state: `sent` counts what went in and `received` what
// came out, both only ever rising, so "has my value been taken" is
// `received >= id` and needs nothing of the value itself.

#include "channel.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "carry.h"
#include "error.h"
#include "lhat/port.h"
#include "port/thread.h"

typedef struct Channel {
    LhatMutex lock;
    LhatCondition changed;

    LhatCarried **items;  // a ring; `head` is the next to come out
    size_t head;
    size_t count;
    size_t capacity;

    uint64_t sent;      // ++ per push; the number a push answers
    uint64_t received;  // ++ per pop; supply waits for it to reach its own

    // 05 の 8.8改2: how many hold this pointer -- a wrapper on some machine,
    // a carried tree carrying it, the table of names. Counted under `lock`;
    // the last to let go frees it.
    int holds;

    // What is inside atomic(): the machine whose call holds `lock`. Every
    // entry below lets that machine straight through rather than taking the
    // lock again (port/thread.h's mutex does not nest), and makes the waits
    // answer without waiting.
    LhatMachine *atomic_owner;
} Channel;

typedef struct {
    const LhatErrorKind *refused;
    const LhatErrorKind *out_of_memory;
    const LhatHostDataTag *tag;
} ChannelModule;

// ---------------------------------------------------------------------------
// The channel itself, which knows nothing about L^
// ---------------------------------------------------------------------------

static Channel *channel_new(void)
{
    Channel *channel = (Channel *)lhat_calloc(1, sizeof *channel);
    if (channel == NULL) {
        return NULL;
    }
    lhat_mutex_init(&channel->lock);
    lhat_condition_init(&channel->changed);
    channel->holds = 1;
    return channel;
}

static void channel_free(Channel *channel)
{
    for (size_t i = 0; i < channel->count; i++) {
        lhat_carried_free(channel->items[(channel->head + i) %
                                         channel->capacity]);
    }
    lhat_free(channel->items);
    lhat_condition_destroy(&channel->changed);
    lhat_mutex_destroy(&channel->lock);
    lhat_free(channel);
}

// Whether this call already holds the lock (it is inside atomic()), so that
// the entry points below neither take it again nor give it back.
static bool holding(const Channel *channel, LhatMachine *machine)
{
    return channel->atomic_owner == machine;
}

static void enter(Channel *channel, LhatMachine *machine)
{
    if (!holding(channel, machine)) {
        lhat_mutex_lock(&channel->lock);
    }
}

static void leave(Channel *channel, LhatMachine *machine)
{
    if (!holding(channel, machine)) {
        lhat_mutex_unlock(&channel->lock);
    }
}

// 05 の 8.8改2's retain/let_go, which is also how the module's own code
// keeps a channel while it is only in the table of names.
static void channel_retain(void *pointer, void *context)
{
    Channel *channel = (Channel *)pointer;
    (void)context;
    lhat_mutex_lock(&channel->lock);
    channel->holds++;
    lhat_mutex_unlock(&channel->lock);
}

static void channel_let_go(void *pointer, void *context)
{
    Channel *channel = (Channel *)pointer;
    (void)context;
    lhat_mutex_lock(&channel->lock);
    bool last = --channel->holds <= 0;
    lhat_mutex_unlock(&channel->lock);
    if (last) {
        channel_free(channel);
    }
}

// Room for one more, the ring unrolled into a fresh array when it grows.
static bool make_room(Channel *channel)
{
    if (channel->count < channel->capacity) {
        return true;
    }
    size_t wanted = channel->capacity > 0 ? channel->capacity * 2 : 8;
    LhatCarried **bigger =
        (LhatCarried **)lhat_calloc(wanted, sizeof *bigger);
    if (bigger == NULL) {
        return false;
    }
    for (size_t i = 0; i < channel->count; i++) {
        bigger[i] = channel->items[(channel->head + i) % channel->capacity];
    }
    lhat_free(channel->items);
    channel->items = bigger;
    channel->capacity = wanted;
    channel->head = 0;
    return true;
}

// The lock is held by the caller throughout the four below.

static bool push_locked(Channel *channel, LhatCarried *item, uint64_t *id)
{
    if (!make_room(channel)) {
        return false;
    }
    channel->items[(channel->head + channel->count) % channel->capacity] =
        item;
    channel->count++;
    *id = ++channel->sent;
    lhat_condition_broadcast(&channel->changed);
    return true;
}

static LhatCarried *pop_locked(Channel *channel)
{
    if (channel->count == 0) {
        return NULL;
    }
    LhatCarried *item = channel->items[channel->head];
    channel->head = (channel->head + 1) % channel->capacity;
    channel->count--;
    channel->received++;
    lhat_condition_broadcast(&channel->changed);
    return item;
}

static void clear_locked(Channel *channel)
{
    while (channel->count > 0) {
        lhat_carried_free(pop_locked(channel));
    }
    // Every supply waiting on this channel is settled rather than left: what
    // it was waiting for is gone, and nothing will ever take it now.
    channel->received = channel->sent;
    lhat_condition_broadcast(&channel->changed);
}

// One wait of at most what is left of `until`, in the loop the caller keeps
// over the thing actually being waited for (port/thread.h). `until` is a
// monotonic deadline (lhat_now_ms), or -1 to wait without one. Answers
// false when the deadline has passed.
static bool wait_a_while(Channel *channel, int64_t until)
{
    if (until < 0) {
        lhat_condition_wait(&channel->changed, &channel->lock);
        return true;
    }
    int64_t left = until - lhat_now_ms();
    if (left <= 0) {
        return false;
    }
    lhat_condition_wait_for(&channel->changed, &channel->lock,
                            left > INT_MAX ? INT_MAX : (int)left);
    return true;
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

// Seconds as a wait in milliseconds, the way std.thread.sleep reads one:
// nothing to wait for (zero, a negative, a NaN) is not an error.
static int64_t milliseconds_of(LhatValue value)
{
    double seconds = lhat_is_integer(value) ? (double)lhat_as_integer(value)
                                            : lhat_as_real(value);
    double ms = seconds * 1000.0;
    if (!(ms > 0.0)) {
        return 0;
    }
    return ms >= (double)INT64_MAX ? INT64_MAX : (int64_t)ms;
}

static Channel *self_of(const ChannelModule *module, LhatValue value)
{
    return (Channel *)lhat_hostdata_pointer(value, module->tag);
}

// A channel as a value of this machine, holding it once more (8.8改2's
// wrapper hold, which its dispose^ gives back).
static bool answer_channel(LhatMachine *machine, const ChannelModule *module,
                           Channel *channel, LhatValue *out)
{
    if (!lhat_machine_make_hostdata(machine, module->tag, channel, out)) {
        return false;
    }
    channel_retain(channel, NULL);
    return true;
}

static void channel_dispose(LhatMachine *machine, void *context,
                            const LhatValue *arguments, size_t count,
                            LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)count;
    (void)answers;
    (void)answer_count;
    // Registered with the tag as its context, not the module: the tag
    // belongs to the process and the module to a program, and a type has
    // one way of handing a value back (stdlib/thread.c says the same).
    const LhatHostDataTag *tag = (const LhatHostDataTag *)context;
    Channel *channel = (Channel *)lhat_hostdata_pointer(arguments[0], tag);
    if (channel != NULL) {
        channel_let_go(channel, NULL);
    }
}

static void channel_make(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count,
                         LhatValue *answers, int *answer_count)
{
    const ChannelModule *module = (const ChannelModule *)context;
    (void)arguments;
    (void)count;
    Channel *channel = channel_new();
    LhatValue out = lhat_nil();
    if (channel == NULL) {
        answers[0] = fail_with(machine, module->out_of_memory,
                               "a channel could not be made");
    } else if (!answer_channel(machine, module, channel, &out)) {
        channel_let_go(channel, NULL);  // the one it was made with
        answers[0] = fail_with(machine, module->out_of_memory,
                               "a channel could not be made");
    } else {
        answers[0] = out;
    }
    *answer_count = 1;
}

// ---------------------------------------------------------------------------
// The names the process shares
// ---------------------------------------------------------------------------
//
// Not in the registry (05 の 8.7改), which is what programs agree about
// before anything runs; this is written while machines run, so it carries
// its own lock. Made on the first registration, which is single-threaded by
// the same rule the registry rests on.

typedef struct {
    char *name;
    Channel *channel;
} Named;

static struct {
    LhatMutex lock;
    bool ready;
    Named *entries;
    size_t count;
    size_t capacity;
} names;

static Channel *named_channel(const char *name, size_t length)
{
    lhat_mutex_lock(&names.lock);
    for (size_t i = 0; i < names.count; i++) {
        if (strlen(names.entries[i].name) == length &&
            memcmp(names.entries[i].name, name, length) == 0) {
            Channel *found = names.entries[i].channel;
            lhat_mutex_unlock(&names.lock);
            return found;
        }
    }
    Channel *made = channel_new();  // the table's own hold
    char *kept = (char *)lhat_alloc(length + 1);
    if (made == NULL || kept == NULL) {
        lhat_free(kept);
        if (made != NULL) {
            channel_let_go(made, NULL);
        }
        lhat_mutex_unlock(&names.lock);
        return NULL;
    }
    memcpy(kept, name, length);
    kept[length] = '\0';
    if (names.count == names.capacity) {
        size_t wanted = names.capacity > 0 ? names.capacity * 2 : 8;
        Named *bigger =
            (Named *)lhat_realloc(names.entries, wanted * sizeof *bigger);
        if (bigger == NULL) {
            lhat_free(kept);
            channel_let_go(made, NULL);
            lhat_mutex_unlock(&names.lock);
            return NULL;
        }
        names.entries = bigger;
        names.capacity = wanted;
    }
    names.entries[names.count].name = kept;
    names.entries[names.count].channel = made;
    names.count++;
    lhat_mutex_unlock(&names.lock);
    return made;
}

void lhatstdlib_channel_forget_named(void)
{
    if (!names.ready) {
        return;
    }
    lhat_mutex_lock(&names.lock);
    for (size_t i = 0; i < names.count; i++) {
        lhat_free(names.entries[i].name);
        Channel *channel = names.entries[i].channel;
        lhat_mutex_lock(&channel->lock);
        clear_locked(channel);
        lhat_mutex_unlock(&channel->lock);
        channel_let_go(channel, NULL);
    }
    lhat_free(names.entries);
    names.entries = NULL;
    names.count = 0;
    names.capacity = 0;
    lhat_mutex_unlock(&names.lock);
}

static void channel_named(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count,
                          LhatValue *answers, int *answer_count)
{
    const ChannelModule *module = (const ChannelModule *)context;
    (void)count;
    const LhatString *name =
        (const LhatString *)lhat_as_object(arguments[0]);
    Channel *channel = named_channel(name->text, name->length);
    LhatValue out = lhat_nil();
    if (channel == NULL || !answer_channel(machine, module, channel, &out)) {
        answers[0] = fail_with(machine, module->out_of_memory,
                               "a channel could not be made");
    } else {
        answers[0] = out;
    }
    *answer_count = 1;
}

// ---------------------------------------------------------------------------
// The members
// ---------------------------------------------------------------------------

// Takes the value apart, or answers the error a refusal is. `*carried` is
// NULL when this answered false.
static bool carry_argument(LhatMachine *machine, const ChannelModule *module,
                           LhatValue value, LhatCarried **carried,
                           LhatValue *error)
{
    const char *refused = NULL;
    if (lhat_carry(value, carried, &refused)) {
        return true;
    }
    *carried = NULL;
    *error = fail_with(machine,
                       refused != NULL &&
                               strcmp(refused, "out of memory") == 0
                           ? module->out_of_memory
                           : module->refused,
                       refused != NULL ? refused : "out of memory");
    return false;
}

static void channel_push(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count,
                         LhatValue *answers, int *answer_count)
{
    const ChannelModule *module = (const ChannelModule *)context;
    (void)count;
    Channel *channel = self_of(module, arguments[0]);
    LhatCarried *item = NULL;
    LhatValue error = lhat_nil();
    if (channel == NULL) {
        answers[0] = lhat_nil();
    } else if (!carry_argument(machine, module, arguments[1], &item, &error)) {
        answers[0] = error;
    } else {
        enter(channel, machine);
        uint64_t id = 0;
        bool ok = push_locked(channel, item, &id);
        leave(channel, machine);
        if (!ok) {
            lhat_carried_free(item);
            answers[0] = fail_with(machine, module->out_of_memory,
                                   "the channel could not grow");
        } else {
            answers[0] = lhat_integer((int64_t)id);
        }
    }
    *answer_count = 1;
}

static void channel_supply(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count,
                           LhatValue *answers, int *answer_count)
{
    const ChannelModule *module = (const ChannelModule *)context;
    Channel *channel = self_of(module, arguments[0]);
    LhatCarried *item = NULL;
    LhatValue error = lhat_nil();
    if (channel == NULL) {
        answers[0] = lhat_bool(false);
        *answer_count = 1;
        return;
    }
    if (!carry_argument(machine, module, arguments[1], &item, &error)) {
        answers[0] = error;
        *answer_count = 1;
        return;
    }
    int64_t until = count >= 3 ? lhat_now_ms() + milliseconds_of(arguments[2])
                               : -1;
    enter(channel, machine);
    uint64_t id = 0;
    bool taken = false;
    if (!push_locked(channel, item, &id)) {
        leave(channel, machine);
        lhat_carried_free(item);
        answers[0] = fail_with(machine, module->out_of_memory,
                               "the channel could not grow");
        *answer_count = 1;
        return;
    }
    // Inside atomic() there is nobody who could take it while this call
    // holds the lock, so the wait is not one -- channel.h says so.
    while (!holding(channel, machine)) {
        if (channel->received >= id) {
            taken = true;
            break;
        }
        if (!wait_a_while(channel, until)) {
            break;
        }
    }
    leave(channel, machine);
    answers[0] = lhat_bool(taken);
    *answer_count = 1;
}

// What a pop answered, built on this machine. The tree is freed either way.
static LhatValue rebuild(LhatMachine *machine, LhatCarried *item)
{
    LhatValue out = lhat_nil();
    if (item == NULL) {
        return out;
    }
    if (!lhat_uncarry(machine, item, &out)) {
        out = lhat_nil();
    }
    lhat_carried_free(item);
    return out;
}

static void channel_pop(LhatMachine *machine, void *context,
                        const LhatValue *arguments, size_t count,
                        LhatValue *answers, int *answer_count)
{
    const ChannelModule *module = (const ChannelModule *)context;
    (void)count;
    Channel *channel = self_of(module, arguments[0]);
    LhatCarried *item = NULL;
    if (channel != NULL) {
        enter(channel, machine);
        item = pop_locked(channel);
        leave(channel, machine);
    }
    answers[0] = rebuild(machine, item);
    *answer_count = 1;
}

static void channel_demand(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count,
                           LhatValue *answers, int *answer_count)
{
    const ChannelModule *module = (const ChannelModule *)context;
    Channel *channel = self_of(module, arguments[0]);
    LhatCarried *item = NULL;
    if (channel != NULL) {
        int64_t until = count >= 2
                            ? lhat_now_ms() + milliseconds_of(arguments[1])
                            : -1;
        enter(channel, machine);
        for (;;) {
            item = pop_locked(channel);
            if (item != NULL || holding(channel, machine)) {
                break;
            }
            if (!wait_a_while(channel, until)) {
                break;
            }
        }
        leave(channel, machine);
    }
    answers[0] = rebuild(machine, item);
    *answer_count = 1;
}

static void channel_peek(LhatMachine *machine, void *context,
                         const LhatValue *arguments, size_t count,
                         LhatValue *answers, int *answer_count)
{
    const ChannelModule *module = (const ChannelModule *)context;
    (void)count;
    Channel *channel = self_of(module, arguments[0]);
    LhatValue out = lhat_nil();
    if (channel != NULL) {
        enter(channel, machine);
        // Built with the lock held: the tree stays in the queue, and no L^
        // runs inside lhat_uncarry for anyone else to reach the channel.
        if (channel->count > 0 &&
            !lhat_uncarry(machine, channel->items[channel->head], &out)) {
            out = lhat_nil();
        }
        leave(channel, machine);
    }
    answers[0] = out;
    *answer_count = 1;
}

static void channel_count(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count,
                          LhatValue *answers, int *answer_count)
{
    const ChannelModule *module = (const ChannelModule *)context;
    (void)count;
    Channel *channel = self_of(module, arguments[0]);
    int64_t held = 0;
    if (channel != NULL) {
        enter(channel, machine);
        held = (int64_t)channel->count;
        leave(channel, machine);
    }
    answers[0] = lhat_integer(held);
    *answer_count = 1;
}

static void channel_has_read(LhatMachine *machine, void *context,
                             const LhatValue *arguments, size_t count,
                             LhatValue *answers, int *answer_count)
{
    const ChannelModule *module = (const ChannelModule *)context;
    (void)count;
    Channel *channel = self_of(module, arguments[0]);
    bool read = false;
    if (channel != NULL) {
        int64_t id = lhat_is_integer(arguments[1])
                         ? lhat_as_integer(arguments[1])
                         : (int64_t)lhat_as_real(arguments[1]);
        enter(channel, machine);
        read = id <= 0 || channel->received >= (uint64_t)id;
        leave(channel, machine);
    }
    answers[0] = lhat_bool(read);
    *answer_count = 1;
}

static void channel_clear(LhatMachine *machine, void *context,
                          const LhatValue *arguments, size_t count,
                          LhatValue *answers, int *answer_count)
{
    const ChannelModule *module = (const ChannelModule *)context;
    (void)count;
    (void)answers;
    (void)answer_count;
    Channel *channel = self_of(module, arguments[0]);
    if (channel != NULL) {
        enter(channel, machine);
        clear_locked(channel);
        leave(channel, machine);
    }
}

static void channel_atomic(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count,
                           LhatValue *answers, int *answer_count)
{
    const ChannelModule *module = (const ChannelModule *)context;
    (void)count;
    (void)answers;
    (void)answer_count;
    Channel *channel = self_of(module, arguments[0]);
    if (channel == NULL) {
        return;
    }
    // Already inside one: the lock is this machine's, so the body simply
    // runs. Nesting a hold would be giving back a lock somebody else's call
    // is holding.
    bool outermost = !holding(channel, machine);
    if (outermost) {
        lhat_mutex_lock(&channel->lock);
        channel->atomic_owner = machine;
    }
    LhatValue self = arguments[0];
    LhatRunResult ran = lhat_machine_call(machine, arguments[1], &self, 1);
    (void)ran;  // a fault ends the run this host function was called from
    if (outermost) {
        channel->atomic_owner = NULL;
        lhat_mutex_unlock(&channel->lock);
    }
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

bool lhatstdlib_channel_register(LhatProgram *program)
{
    if (!lhatstdlib_error_register(program)) {
        return false;
    }
    if (!names.ready) {
        // 05 の 8.7: registration is before anything runs, which is what
        // makes this the one moment a process-wide lock may be made.
        lhat_mutex_init(&names.lock);
        names.ready = true;
    }

    ChannelModule *module = (ChannelModule *)lhat_calloc(1, sizeof *module);
    if (module == NULL) {
        return false;
    }
    if (!lhat_program_on_dispose(program, lhat_free, module)) {
        lhat_free(module);
        return false;
    }
    module->out_of_memory = lhatstdlib_error_lookup(program, "OutOfMemory");

    static const char *const variants[] = {"Refused"};
    const LhatErrorKind *kinds[1];
    if (!lhat_register_error_kind(program, "std.channel", "ChannelError",
                                  variants, 1, NULL, kinds)) {
        return false;
    }
    module->refused = kinds[0];

    module->tag =
        lhat_register_hostdata_type(program, "std.channel", "Channel");
    if (module->tag == NULL) {
        return false;
    }

#define REFUSAL \
    "|std.channel.ChannelError.Refused|std.error.OutOfMemory;"

    return lhat_register_func(program, "std.channel", "new",
                              "p^ -> std.channel.Channel"
                              "|std.error.OutOfMemory;",
                              channel_make, module) &&
           lhat_register_func(program, "std.channel", "named",
                              "p^string^ -> std.channel.Channel"
                              "|std.error.OutOfMemory;",
                              channel_named, module) &&
           lhat_register_member(program, "std.channel", "Channel", "push",
                                "p^self^, any^ -> number^" REFUSAL,
                                channel_push, module) &&
           // 14.12: the two shapes of a wait are two arms of one name, the
           // way a host writes an optional argument (love does the same).
           lhat_register_member(program, "std.channel", "Channel", "supply",
                                "p^self^, any^ -> bool^" REFUSAL,
                                channel_supply, module) &&
           lhat_register_member(program, "std.channel", "Channel", "supply",
                                "p^self^, any^, number^ -> bool^" REFUSAL,
                                channel_supply, module) &&
           lhat_register_member(program, "std.channel", "Channel", "pop",
                                "p^self^ -> any^;", channel_pop, module) &&
           lhat_register_member(program, "std.channel", "Channel", "demand",
                                "p^self^ -> any^;", channel_demand, module) &&
           lhat_register_member(program, "std.channel", "Channel", "demand",
                                "p^self^, number^ -> any^;", channel_demand,
                                module) &&
           lhat_register_member(program, "std.channel", "Channel", "peek",
                                "p^self^ -> any^;", channel_peek, module) &&
           lhat_register_member(program, "std.channel", "Channel", "count",
                                "f^self^ -> number^;", channel_count,
                                module) &&
           lhat_register_member(program, "std.channel", "Channel", "hasRead",
                                "f^self^, number^ -> bool^;",
                                channel_has_read, module) &&
           lhat_register_member(program, "std.channel", "Channel", "clear",
                                "p^self^;", channel_clear, module) &&
           // The body takes the channel and nothing else: what else it wants
           // it closes over, which a fixed-arity closure cannot do through a
           // variadic signature.
           lhat_register_member(program, "std.channel", "Channel", "atomic",
                                "p^self^, p^std.channel.Channel;;",
                                channel_atomic, module) &&
           lhat_register_member(program, "std.channel", "Channel", "dispose",
                                "p^self^;", channel_dispose,
                                (void *)module->tag) &&
           // 8.8改2: a channel crosses machines as its pointer, which is
           // what lets one be pushed into another or handed to spawn.
           lhat_register_hostdata_shared(program, "std.channel", "Channel",
                                         channel_retain, channel_let_go,
                                         NULL);

#undef REFUSAL
}
