// L^ (lhat) -- minimal assertion harness for the test executables.
//
// Deliberately dependency free: the project builds with nothing but a C11
// compiler, and the test suite should not change that.

#ifndef LHAT_TESTUTIL_H
#define LHAT_TESTUTIL_H

#include <stdio.h>
#include <string.h>

static int lhat_test_failures = 0;
static int lhat_test_checks = 0;
static const char *lhat_test_current = "";

#define LHAT_TEST(name)                     \
    do {                                    \
        lhat_test_current = (name);         \
    } while (0)

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

#define LHAT_CHECK_EQ_INT(actual, expected)                               \
    do {                                                                  \
        long long a_ = (long long)(actual);                               \
        long long e_ = (long long)(expected);                             \
        LHAT_CHECK(a_ == e_, "%s == %s: got %lld, want %lld",             \
                   #actual, #expected, a_, e_);                           \
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

static int lhat_test_report(const char *suite)
{
    if (lhat_test_failures == 0) {
        printf("%s: %d checks passed\n", suite, lhat_test_checks);
        return 0;
    }
    printf("%s: %d of %d checks FAILED\n", suite, lhat_test_failures,
           lhat_test_checks);
    return 1;
}

#endif  // LHAT_TESTUTIL_H
