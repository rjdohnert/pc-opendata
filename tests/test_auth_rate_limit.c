#include "database_engine.h"
#include <assert.h>

int main(void) {
    reset_auth_rate_limit();
    for (unsigned i = 0; i < 5; i++)
        assert(admit_auth_attempt(1, "admin", 0));
    assert(!admit_auth_attempt(1, "admin", 0));
    assert(admit_auth_attempt(1, "other-account", 0));
    assert(admit_auth_attempt(2, "admin", 0));
    assert(!admit_auth_attempt(1, "admin", 4999));
    assert(admit_auth_attempt(1, "admin", 5000));
    assert(!admit_auth_attempt(1, "admin", 5000));
    assert(!admit_auth_attempt(1, "admin", 4999));
    assert(admit_auth_attempt(1, "admin", 65000));

    reset_auth_rate_limit();
    for (unsigned i = 0; i < 20; i++) {
        char user[32];
        snprintf(user, sizeof(user), "unknown-%u", i);
        assert(admit_auth_attempt(1, user, 0));
    }
    assert(!admit_auth_attempt(1, "new-name", 0));
    assert(admit_auth_attempt(2, "admin", 0));
    assert(!admit_auth_attempt(1, "new-name", 1999));
    assert(admit_auth_attempt(1, "new-name", 2000));
    assert(auth_rate_limit_stats().source_throttled == 2);

    reset_auth_rate_limit();
    for (unsigned i = 0; i < 64; i++)
        assert(admit_auth_attempt(i, "admin", 0));
    assert(!admit_auth_attempt(100, "admin", 99));
    assert(admit_auth_attempt(100, "admin", 100));
    assert(auth_rate_limit_stats().global_throttled == 1);

    reset_auth_rate_limit();
    for (unsigned i = 0; i < 256; i++)
        assert(admit_auth_attempt(i, "admin", (uint64_t)i * 100));
    assert(!admit_auth_attempt(256, "admin", 25600));
    assert(auth_rate_limit_stats().source_saturated == 1);
    assert(admit_auth_attempt(0, "admin", 25600));
    assert(admit_auth_attempt(256, "admin", 85601));

    reset_auth_rate_limit();
    for (unsigned i = 0; i < 5; i++)
        assert(admit_auth_attempt(7, "disabled-or-unknown", 0));
    for (unsigned i = 0; i < 15; i++) {
        char user[32];
        snprintf(user, sizeof(user), "churn-%u", i);
        assert(admit_auth_attempt(7, user, 0));
    }
    assert(!admit_auth_attempt(7, "disabled-or-unknown", 0));
    assert(admit_auth_attempt(8, "disabled-or-unknown", 0));

    reset_auth_rate_limit();
    for (unsigned i = 0; i < 512; i++) {
        char user[32];
        snprintf(user, sizeof(user), "pair-%u", i);
        assert(admit_auth_attempt(i % 64, user, (uint64_t)i * 100));
    }
    assert(!admit_auth_attempt(100, "new-pair", 51200));
    assert(admit_auth_attempt(0, "pair-0", 51200));
    assert(!admit_auth_attempt(100, "new-pair", 59999));
    assert(admit_auth_attempt(100, "new-pair", 60100));
    AuthRateLimitStats stats = auth_rate_limit_stats();
    assert(stats.pair_saturated == 2);
    assert(stats.admitted == 514);
    assert(stats.credential_failures == 0);
    record_auth_credential_failure();
    assert(auth_rate_limit_stats().credential_failures == 1);
    assert(stats.credential_failures == 0);
    reset_auth_rate_limit();
    assert(auth_rate_limit_stats().admitted == 0);
    for (unsigned i = 0; i < 5; i++)
        assert(admit_auth_attempt(1, "same-nat-account", 0));
    assert(!admit_auth_attempt(1, "same-nat-account", 0));
    assert(auth_rate_limit_stats().pair_throttled == 1);
    assert(admit_auth_attempt(2, "same-nat-account", 0));
    puts("Authentication admission limits passed.");
    return 0;
}
