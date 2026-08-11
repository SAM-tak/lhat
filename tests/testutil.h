// L^ (lhat) -- minimal assertion harness for the test executables.
//
// Deliberately dependency free: the project builds with nothing but a C11
// compiler, and the test suite should not change that. The counters live in
// testutil.c so that a test executable may span more than one translation
// unit; every test target links the testkit library.

#ifndef LHAT_TESTUTIL_H
#define LHAT_TESTUTIL_H

#include <math.h>
#include <stdio.h>
#include <string.h>

extern int lhat_test_failures;
extern int lhat_test_checks;
extern const char *lhat_test_current;

#define LHAT_TEST(name)                     \
    do {                                    \
        lhat_test_current = (name);         \
    } while (0)

// Records a failure and carries on, so that one run reports everything that
// is wrong rather than stopping at the first.
//
// The cost is that a check cannot guard what follows it. A pointer this has
// just found to be NULL is still NULL on the next line -- anything that
// would then read through it belongs under LHAT_REQUIRE instead.
#define LHAT_CHECK(condition, ...)                                        \
    do {                                                                  \
        lhat_test_checks++;                                               \
        if (!(condition)) {                                               \
            lhat_test_failures++;                                         \
            printf("FAIL [%s] %s:%d: ", lhat_test_current, __FILE__, __LINE__); \
            printf(__VA_ARGS__);                                          \
            printf("\n");                                                 \
        }                                                                 \
    } while (0)

// The check the rest of the test cannot survive failing: a NULL about to be
// read through, a fixture that did not build. Records the failure the same
// way and returns from the enclosing test function.
#define LHAT_REQUIRE(condition, ...)                                      \
    do {                                                                  \
        lhat_test_checks++;                                               \
        if (!(condition)) {                                               \
            lhat_test_failures++;                                         \
            printf("FAIL [%s] %s:%d: ", lhat_test_current, __FILE__, __LINE__); \
            printf(__VA_ARGS__);                                          \
            printf("\n");                                                 \
            return;                                                       \
        }                                                                 \
    } while (0)

// The typed comparisons print both sides on failure, which the raw form
// cannot. Prefer these wherever the values have a printable type.

#define LHAT_CHECK_EQ_INT(actual, expected)                               \
    do {                                                                  \
        long long a_ = (long long)(actual);                               \
        long long e_ = (long long)(expected);                             \
        LHAT_CHECK(a_ == e_, "%s == %s: got %lld, want %lld",             \
                   #actual, #expected, a_, e_);                           \
    } while (0)

#define LHAT_CHECK_EQ_BOOL(actual, expected)                              \
    do {                                                                  \
        int a_ = (actual) ? 1 : 0;                                        \
        int e_ = (expected) ? 1 : 0;                                      \
        LHAT_CHECK(a_ == e_, "%s: got %s, want %s", #actual,              \
                   a_ ? "true" : "false", e_ ? "true" : "false");         \
    } while (0)

#define LHAT_CHECK_EQ_PTR(actual, expected)                               \
    do {                                                                  \
        const void *a_ = (const void *)(actual);                          \
        const void *e_ = (const void *)(expected);                        \
        LHAT_CHECK(a_ == e_, "%s == %s: got %p, want %p",                 \
                   #actual, #expected, a_, e_);                           \
    } while (0)

#define LHAT_CHECK_EQ_REAL(actual, expected, epsilon)                     \
    do {                                                                  \
        double a_ = (double)(actual);                                     \
        double e_ = (double)(expected);                                   \
        LHAT_CHECK(fabs(a_ - e_) <= (epsilon),                            \
                   "%s: got %.17g, want %.17g (within %g)",               \
                   #actual, a_, e_, (double)(epsilon));                   \
    } while (0)

#define LHAT_CHECK_EQ_STR(actual, actual_length, expected)                \
    do {                                                                  \
        size_t al_ = (size_t)(actual_length);                             \
        const char *ap_ = (actual);                                       \
        const char *ep_ = (expected);                                     \
        size_t el_ = strlen(ep_);                                         \
        LHAT_CHECK(al_ == el_ && ap_ != NULL && memcmp(ap_, ep_, el_) == 0, \
                   "%s: got \"%.*s\" (%zu bytes), want \"%s\"",           \
                   #actual, (int)al_, ap_ != NULL ? ap_ : "", al_, ep_);  \
    } while (0)

// Prints the tally and answers the process exit code.
int lhat_test_report(const char *suite);

#endif  // LHAT_TESTUTIL_H
