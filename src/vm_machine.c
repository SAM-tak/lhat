// L^ (lhat) -- machine lifetime, environment and weak cache.

#include "vm_internal.h"
#include <string.h>
#include "environment.h"
#include "lhat/config.h"
#include "lhat/port.h"

// 05 の 8.6: L^ is the one name that is there without being imported, so what
// it answers is made with the machine. A member is added here and its type in
// check.c's environment_type -- the two lists have to say the same thing.
bool vm_set_member(Machine *m, LhatTable *table, const char *name,
                       LhatValue value)
{
    LhatString *key = lhat_string_new(&m->objects, name, strlen(name));
    if (key == NULL) {
        return false;
    }
    bool refused = false;
    return vm_set_key(m, table, lhat_object((LhatObject *)key), value, &refused) &&
           !refused;
}

static bool build_environment(Machine *m)
{
    m->environment = lhat_table_new(&m->objects);
    // 05 の 5.3: the registry a unit is loaded into once. Empty until
    // something is loaded, and grown the way 8.8 grows any table.
    LhatTable *modules_value = lhat_table_new(&m->objects);
    LhatNative *collectgarbage_value =
        lhat_native_new(&m->objects, LHAT_NATIVE_COLLECTGARBAGE, lhat_nil());
    if (m->environment == NULL || modules_value == NULL ||
        collectgarbage_value == NULL) {
        return false;
    }
    // environment.h's one list -- check.c's environment_type expands the
    // same one for the types.
#define LHAT_ENVIRONMENT_SET(name, value, type)               \
    if (!vm_set_member(m, m->environment, #name, (value))) {     \
        return false;                                         \
    }
    LHAT_ENVIRONMENT(LHAT_ENVIRONMENT_SET)
#undef LHAT_ENVIRONMENT_SET
    // 05 の 8.6: sealed once built, not before -- the two members above
    // are the machine writing its own table, which is exactly what the mark
    // goes on to refuse from an instruction.
    //
    // The registry inside it is not marked here. 5.3 has a unit register
    // itself, and what it emits for that is an ordinary instruction writing
    // into L^.modules -- the mark would refuse the machine's own bookkeeping.
    // check.c refuses what a writer spells there, which is the half that can
    // be told apart by looking at the source.
    m->environment->sealed = true;
    m->modules = modules_value;
    return true;
}

// 03 の 4.3改: the three runs sit after the struct in one block, so their
// starts have to land aligned without any padding arithmetic. They do,
// because a type's size is a multiple of its alignment -- asserted rather
// than assumed.
_Static_assert(sizeof(Machine) % _Alignof(Frame) == 0 &&
                   sizeof(Frame) % _Alignof(LhatValueUnion) == 0,
               "what is laid after the machine has to land aligned");

LhatMachine *lhat_machine_new(void)
{
    return lhat_machine_new_with_size(0, 0);
}

LhatMachine *lhat_machine_new_with_size(size_t frames, size_t slots)
{
    if (frames == 0) {
        frames = LHAT_MAX_FRAMES;
    }
    if (slots == 0) {
        slots = LHAT_STACK_SLOTS;
    }
    // The least that can run anything: one frame, and room for the widest
    // body there could be. 256 rather than LHAT_MAX_REGISTERS + 1 because
    // what has to fit is not what a compile makes but what a proto may SAY
    // -- `chunk.registers` and `reserved` are both uint8_t, in a unit read
    // from a file as readily as one just compiled. At 256 every value of
    // those fields fits, so no run has to be turned away for being wider
    // than the machine, and no read can reach past the runs.
    if (frames < 1) {
        frames = 1;
    }
    if (slots < 256) {
        slots = 256;
    }
    // Far past anything a caller means, and the point of saying so is that
    // the arithmetic below must not wrap on the way to a refusal.
    if (frames > (size_t)1 << 16 || slots > (size_t)1 << 24) {
        return NULL;
    }

    // A whole stack and a frame array, so the heap is where it belongs --
    // a static one could serve only one caller and never nest.
    // calloc rather than malloc, and nothing more: 03 の 2.2 numbers
    // LHAT_VALUE_NIL first and gives it a zero payload, so zeroed memory is
    // already a stack full of nil^.
    //
    // 4.3改: one block for the struct and the three runs together. A machine
    // is still one allocation and one free, and the runs are as near the
    // fields that walk them as the fixed arrays were.
    size_t bytes = sizeof(Machine) + frames * sizeof(Frame) +
                   slots * sizeof(LhatValueUnion) + slots;
    Machine *m = (Machine *)lhat_calloc(1, bytes);
    if (m == NULL) {
        return NULL;
    }
    m->frames = (Frame *)((char *)m + sizeof *m);
    m->frame_capacity = frames;
    // 2.2: the one view every register read goes through, fixed for the
    // machine's whole life. Tag zero is nil^, so the zeroed runs are already
    // a stack full of it.
    LhatValueUnion *values = (LhatValueUnion *)(m->frames + frames);
    m->slots.values = values;
    m->slots.tags = (uint8_t *)(values + slots);
    m->slot_capacity = slots;
    m->threshold = LHAT_GC_INITIAL_THRESHOLD;
    if (!build_environment(m)) {
        lhat_machine_dispose(m);
        return NULL;
    }
    // 02 の 14.11: the key every construction reads the prototype through.
    m->self_key = lhat_string_new(&m->objects, "self^", 5);
    if (m->self_key == NULL) {
        lhat_machine_dispose(m);
        return NULL;
    }
    // 09 の 5.1: a debugger following every machine hears of this one here,
    // before it has run anything.
#if LHAT_WITH_DEBUGGER
    if (lhat_machine_watcher.born != NULL) {
        lhat_machine_watcher.born(lhat_machine_watcher.context,
                                  (LhatMachine *)m);
    }
#endif
    return m;
}
void lhat_machine_dispose(LhatMachine *machine)
{
    if (machine == NULL) {
        return;
    }
    // 09 の 5.1: before anything else -- a watcher takes its hook off here,
    // and whatever the disposal below still runs must not sound it.
#if LHAT_WITH_DEBUGGER
    if (lhat_machine_watcher.dying != NULL) {
        lhat_machine_watcher.dying(lhat_machine_watcher.context, machine);
    }
#endif
    // 05 の 8.8: what the host made goes back before anything is freed, so a
    // release may still read the value it is given. Reachability is not asked
    // -- the machine is going, so everything on it is.
    for (LhatObject *object = machine->objects.objects; object != NULL;
         object = object->next) {
        lhat_hostdata_release(object, machine);
        lhat_coroutine_release(object, machine);
    }
    lhat_object_free_all(&machine->objects);
    // 05 の 8.9: the tables were the heap's (freed just above); the array
    // alone is the machine's.
    lhat_free(machine->weak);
    lhat_free(machine->hostvalue_members);
    lhat_free(machine);
}
bool lhat_machine_holds_body(const LhatMachine *machine,
                             const LhatProto *const *bodies, size_t count)
{
    if (machine == NULL || bodies == NULL || count == 0) {
        return false;
    }
    // The heap's chain holds every object the machine has, live or not yet
    // swept -- which is why the collector goes first (vm.h). Closures name
    // their body outright; a coroutine names it through the closure it was
    // suspended in (a host walk has none). Everything else holds no body.
    for (const LhatObject *object = machine->objects.objects; object != NULL;
         object = object->next) {
        const LhatProto *proto = NULL;
        if (object->kind == LHAT_OBJECT_SUBROUTINE) {
            proto = ((const LhatClosure *)object)->proto;
        } else if (object->kind == LHAT_OBJECT_COROUTINE) {
            const LhatCoroutine *coroutine = (const LhatCoroutine *)object;
            proto =
                coroutine->closure != NULL ? coroutine->closure->proto : NULL;
        }
        if (proto == NULL) {
            continue;
        }
        for (size_t i = 0; i < count; i++) {
            if (bodies[i] == proto) {
                return true;
            }
        }
    }
    return false;
}
// ---------------------------------------------------------------------------
// 05 の 8.12: the weak cache
// ---------------------------------------------------------------------------

// An address, spread over the word. The low bits of a malloc'd pointer are
// zero (alignment), so they are the last thing an index should be taken
// from.
static size_t weak_hash(const void *key)
{
    uint64_t bits = (uint64_t)(uintptr_t)key;
    bits ^= bits >> 33;
    bits *= 0xff51afd7ed558ccdULL;
    bits ^= bits >> 29;
    return (size_t)bits;
}

// The slot `key` belongs in: the one holding it, or the first free one on
// its probe. NULL when the table has no room at all. A removed slot is
// remembered and answered where the key is not there, so a put fills it in
// again rather than growing past it.
static LhatWeakEntry *weak_slot(LhatWeakEntry *entries, size_t capacity,
                                const void *key)
{
    if (capacity == 0) {
        return NULL;
    }
    size_t mask = capacity - 1;
    size_t at = weak_hash(key) & mask;
    LhatWeakEntry *gone = NULL;
    for (size_t step = 0; step < capacity; step++) {
        LhatWeakEntry *entry = &entries[(at + step) & mask];
        if (entry->key == key) {
            return entry;
        }
        if (entry->key == NULL) {
            return gone != NULL ? gone : entry;
        }
        if (entry->key == LHAT_WEAK_GONE && gone == NULL) {
            gone = entry;
        }
    }
    return gone;
}

static bool weak_grow(Machine *m)
{
    size_t wanted = m->weak_capacity > 0 ? m->weak_capacity * 2 : 16;
    LhatWeakEntry *bigger =
        (LhatWeakEntry *)lhat_calloc(wanted, sizeof *bigger);
    if (bigger == NULL) {
        return false;
    }
    for (size_t i = 0; i < m->weak_capacity; i++) {
        const void *key = m->weak[i].key;
        if (key == NULL || key == LHAT_WEAK_GONE) {
            continue;
        }
        LhatWeakEntry *into = weak_slot(bigger, wanted, key);
        into->key = key;
        into->value = m->weak[i].value;
    }
    lhat_free(m->weak);
    m->weak = bigger;
    m->weak_capacity = wanted;
    m->weak_used = m->weak_count;
    return true;
}

LhatValue lhat_machine_weak_cache_get(LhatMachine *machine, const void *key)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || key == NULL || key == LHAT_WEAK_GONE) {
        return lhat_nil();
    }
    LhatWeakEntry *entry = weak_slot(m->weak, m->weak_capacity, key);
    if (entry == NULL || entry->key != key) {
        return lhat_nil();
    }
    // Asking for it is what makes it reachable again. Only while a marking
    // is under way: in the sweep the entry would already be gone if it had
    // been decided against, and outside a cycle there is no colour to fix.
    if (m->gcstate == LHAT_GC_PROPAGATE) {
        lhat_gc_reach(&m->gray, entry->value);
    }
    return entry->value;
}

bool lhat_machine_weak_cache_put(LhatMachine *machine, const void *key,
                                 LhatValue value)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || key == NULL || key == LHAT_WEAK_GONE) {
        return false;
    }
    if (lhat_is_nil(value)) {
        lhat_machine_weak_cache_forget(machine, key);
        return true;
    }
    // Three quarters full counting what removals left, since those are what
    // a probe still has to step over.
    if ((m->weak_used + 1) * 4 >= m->weak_capacity * 3 && !weak_grow(m)) {
        return false;
    }
    LhatWeakEntry *entry = weak_slot(m->weak, m->weak_capacity, key);
    if (entry == NULL) {
        return false;
    }
    if (entry->key != key) {
        if (entry->key == NULL) {
            m->weak_used++;
        }
        m->weak_count++;
        entry->key = key;
    }
    entry->value = value;
    return true;
}

void lhat_machine_weak_cache_forget(LhatMachine *machine, const void *key)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || key == NULL || key == LHAT_WEAK_GONE) {
        return;
    }
    LhatWeakEntry *entry = weak_slot(m->weak, m->weak_capacity, key);
    if (entry == NULL || entry->key != key) {
        return;
    }
    entry->key = LHAT_WEAK_GONE;  // the probe past it still has to work
    entry->value = lhat_nil();
    m->weak_count--;
}

void lhat_machine_set_budget(LhatMachine *machine, int64_t turns)
{
    Machine *m = (Machine *)machine;
    if (m != NULL) {
        m->budget = turns > 0 ? turns : 0;
    }
}

bool lhat_machine_is_suspended(const LhatMachine *machine)
{
    const Machine *m = (const Machine *)machine;
    return m != NULL && m->suspended;
}
