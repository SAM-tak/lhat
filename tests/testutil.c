// L^ (lhat) -- the test harness's one translation unit: the counters and
// the tally. Everything else in testutil.h is a macro, so this is what a
// multi-file test executable shares.

#include "testutil.h"

int lhat_test_failures = 0;
int lhat_test_checks = 0;
const char *lhat_test_current = "";

int lhat_test_report(const char *suite)
{
    if (lhat_test_failures == 0) {
        printf("%s: %d checks passed\n", suite, lhat_test_checks);
        return 0;
    }
    printf("%s: %d of %d checks FAILED\n", suite, lhat_test_failures,
           lhat_test_checks);
    return 1;
}
