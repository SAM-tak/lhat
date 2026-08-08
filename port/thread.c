// L^ (lhat) -- an OS thread, a lock and a condition, on whichever system.
//
// Two implementations of one header, chosen by the preprocessor and nothing
// else. See port/thread.h for why neither of them is C11's <threads.h>.

// clock_gettime and CLOCK_REALTIME are the one thing below that <time.h>
// keeps behind a feature test, and CMAKE_C_EXTENSIONS is OFF (-std=c11, not
// -std=gnu11) so nothing defines one for us. It is asked for here rather than
// on the target because port/thread.h itself needs none of it: pthread.h's own
// declarations are not guarded this way.
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "thread.h"

#ifdef _WIN32

#include <process.h>
#include <stdint.h>

// _beginthreadex rather than CreateThread: a thread started by the latter has
// no CRT state of its own, so anything it calls that keeps some (strerror,
// strtok, and the allocator on some runtimes) leaks it at exit. Every thread
// started through this header runs code that allocates.
static unsigned __stdcall trampoline(void *raw)
{
    LhatThread *thread = (LhatThread *)raw;
    return (unsigned)thread->entry(thread->argument);
}

bool lhat_thread_start(LhatThread *thread, int (*entry)(void *),
                       void *argument)
{
    thread->entry = entry;
    thread->argument = argument;
    thread->started = false;
    // The write above has to be visible to the new thread; starting one is
    // itself the ordering that makes it so, on either system.
    uintptr_t started =
        _beginthreadex(NULL, 0, trampoline, thread, 0, NULL);
    if (started == 0) {
        return false;
    }
    thread->handle = (HANDLE)started;
    thread->started = true;
    return true;
}

void lhat_thread_join(LhatThread *thread)
{
    if (!thread->started) {
        return;
    }
    WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);
    thread->started = false;
}

void lhat_mutex_init(LhatMutex *mutex) { InitializeSRWLock(&mutex->lock); }

// An SRWLOCK holds nothing that has to be given back.
void lhat_mutex_destroy(LhatMutex *mutex) { (void)mutex; }

void lhat_mutex_lock(LhatMutex *mutex) { AcquireSRWLockExclusive(&mutex->lock); }

void lhat_mutex_unlock(LhatMutex *mutex)
{
    ReleaseSRWLockExclusive(&mutex->lock);
}

void lhat_condition_init(LhatCondition *condition)
{
    InitializeConditionVariable(&condition->signal);
}

void lhat_condition_destroy(LhatCondition *condition) { (void)condition; }

void lhat_condition_wait(LhatCondition *condition, LhatMutex *mutex)
{
    SleepConditionVariableSRW(&condition->signal, &mutex->lock, INFINITE, 0);
}

void lhat_condition_wait_for(LhatCondition *condition, LhatMutex *mutex,
                             int milliseconds)
{
    if (milliseconds < 0) {
        milliseconds = 0;
    }
    SleepConditionVariableSRW(&condition->signal, &mutex->lock,
                              (DWORD)milliseconds, 0);
}

void lhat_condition_signal(LhatCondition *condition)
{
    WakeConditionVariable(&condition->signal);
}

void lhat_condition_broadcast(LhatCondition *condition)
{
    WakeAllConditionVariable(&condition->signal);
}

#else  // POSIX

#include <errno.h>
#include <time.h>

static void *trampoline(void *raw)
{
    LhatThread *thread = (LhatThread *)raw;
    thread->entry(thread->argument);
    return NULL;
}

bool lhat_thread_start(LhatThread *thread, int (*entry)(void *),
                       void *argument)
{
    thread->entry = entry;
    thread->argument = argument;
    thread->started = false;
    if (pthread_create(&thread->handle, NULL, trampoline, thread) != 0) {
        return false;
    }
    thread->started = true;
    return true;
}

void lhat_thread_join(LhatThread *thread)
{
    if (!thread->started) {
        return;
    }
    pthread_join(thread->handle, NULL);
    thread->started = false;
}

void lhat_mutex_init(LhatMutex *mutex)
{
    pthread_mutex_init(&mutex->lock, NULL);
}

void lhat_mutex_destroy(LhatMutex *mutex) { pthread_mutex_destroy(&mutex->lock); }

void lhat_mutex_lock(LhatMutex *mutex) { pthread_mutex_lock(&mutex->lock); }

void lhat_mutex_unlock(LhatMutex *mutex) { pthread_mutex_unlock(&mutex->lock); }

void lhat_condition_init(LhatCondition *condition)
{
    pthread_cond_init(&condition->signal, NULL);
}

void lhat_condition_destroy(LhatCondition *condition)
{
    pthread_cond_destroy(&condition->signal);
}

void lhat_condition_wait(LhatCondition *condition, LhatMutex *mutex)
{
    pthread_cond_wait(&condition->signal, &mutex->lock);
}

void lhat_condition_wait_for(LhatCondition *condition, LhatMutex *mutex,
                             int milliseconds)
{
    if (milliseconds < 0) {
        milliseconds = 0;
    }
    // pthread_cond_timedwait wants an absolute deadline on the same clock the
    // condition was made with, which is CLOCK_REALTIME by default -- so the
    // relative wait this header offers is turned into one here rather than at
    // each call site.
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += milliseconds / 1000;
    deadline.tv_nsec += (long)(milliseconds % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec += 1;
    }
    pthread_cond_timedwait(&condition->signal, &mutex->lock, &deadline);
}

void lhat_condition_signal(LhatCondition *condition)
{
    pthread_cond_signal(&condition->signal);
}

void lhat_condition_broadcast(LhatCondition *condition)
{
    pthread_cond_broadcast(&condition->signal);
}

#endif
