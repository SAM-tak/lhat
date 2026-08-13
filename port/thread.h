// L^ (lhat) -- an OS thread, a lock and a condition, on whichever system.
//
// The core asks for none of this: src/port.h is what the *language* needs from
// its surroundings, and the language runs on one thread by itself (src/gc.c's
// "one set of roots rather than a thread apiece"). This is what things built
// *beside* the core ask for -- stdlib/thread.c, which starts a machine of its
// own on a thread of its own, and lsp/, which keeps one worker off the read
// loop. So it lives in port/ and is a target of its own that `lhat` does not
// link: leave lhatthread out and the language is unaffected.
//
// **Why not C11 <threads.h>.** It is optional (__STDC_NO_THREADS__), and the
// systems that skip it are not obscure ones:
//
//   - Apple's libc has never shipped <threads.h> at all, so every macOS build
//     of anything using it fails outright.
//   - MSVC's UCRT grew one only in Visual Studio 2022 17.8 (2023).
//   - glibc has had it since 2.28 (2018), musl and FreeBSD longer.
//
// pthreads and the Win32 synchronisation API are both older than any of that
// and are present wherever a thread is. So the two of them are what this is,
// and <threads.h> is not used anywhere in the repository.
//
// The shape stays C11's, because that is what the callers were already
// written against -- an entry point is `int (*)(void *)` and answers a code
// nobody reads. The one deliberate difference is that a timed wait takes a
// *relative* number of milliseconds rather than an absolute deadline: every
// caller here wants "no longer than this", and each of the two systems below
// then does the conversion its own API wants instead of the caller doing it.
//
// None of it is a cancellation or a detach facility. A thread here is started
// and joined, and stdlib/thread.c's comment on join_and_free says why letting
// one outlive the handle that names it is not offered.

#ifndef LHAT_PORT_THREAD_H
#define LHAT_PORT_THREAD_H

#include <stdbool.h>

#ifdef _WIN32
// The three of these keep <windows.h> to the synchronisation API and out of
// the way of ordinary names: NOMINMAX and NOGDI are what otherwise take `min`,
// `max` and `ERROR`.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef NOGDI
#define NOGDI
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

// A started thread, and what it was started on. The entry and its argument are
// kept here rather than in an allocation of their own because both systems'
// entry points have a shape of their own to trampoline through, and because
// this struct already outlives the thread everywhere it is used -- a thread is
// joined through the same object that started it.
typedef struct {
#ifdef _WIN32
    HANDLE handle;
#else
    pthread_t handle;
#endif
    int (*entry)(void *);
    void *argument;
    bool started;
} LhatThread;

typedef struct {
#ifdef _WIN32
    // Vista and later. Smaller and faster than a CRITICAL_SECTION and needs no
    // disposal, which is why lhat_mutex_destroy has nothing to do there.
    SRWLOCK lock;
#else
    pthread_mutex_t lock;
#endif
} LhatMutex;

typedef struct {
#ifdef _WIN32
    CONDITION_VARIABLE signal;
#else
    pthread_cond_t signal;
#endif
} LhatCondition;

// Answers false when the system refused to start one, leaving `thread`
// untouched by any join. Otherwise the thread is running and owes exactly one
// lhat_thread_join.
bool lhat_thread_start(LhatThread *thread, int (*entry)(void *),
                       void *argument);

// Blocks until the thread has finished and releases what held it. Whatever the
// entry answered is dropped -- every caller here carries its result out through
// memory it shares with the thread, which is what makes the ordering this
// establishes worth more than the code would be.
void lhat_thread_join(LhatThread *thread);

// Blocks the calling thread for this long, in the relative milliseconds the
// waits above are written in. Zero or less returns at once, and a signal
// arriving part way through does not shorten it.
void lhat_thread_sleep(int milliseconds);

void lhat_mutex_init(LhatMutex *mutex);
void lhat_mutex_destroy(LhatMutex *mutex);
void lhat_mutex_lock(LhatMutex *mutex);
void lhat_mutex_unlock(LhatMutex *mutex);

void lhat_condition_init(LhatCondition *condition);
void lhat_condition_destroy(LhatCondition *condition);

// Both of these are woken spuriously, the same as the primitives underneath
// them, so both belong in a loop over the thing actually being waited for.
void lhat_condition_wait(LhatCondition *condition, LhatMutex *mutex);
void lhat_condition_wait_for(LhatCondition *condition, LhatMutex *mutex,
                             int milliseconds);

void lhat_condition_signal(LhatCondition *condition);
void lhat_condition_broadcast(LhatCondition *condition);

#endif  // LHAT_PORT_THREAD_H
