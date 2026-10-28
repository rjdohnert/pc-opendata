#include "database_engine.h"

#define AUTH_SOURCE_BUCKETS 256
#define AUTH_PAIR_BUCKETS 512
#define AUTH_BUCKET_IDLE_MS 60000u
#define AUTH_SOURCE_INTERVAL_MS 2000u
#define AUTH_PAIR_INTERVAL_MS 5000u
#define AUTH_GLOBAL_INTERVAL_MS 100u
#define AUTH_SOURCE_CAPACITY_MS (20u * AUTH_SOURCE_INTERVAL_MS)
#define AUTH_PAIR_CAPACITY_MS (5u * AUTH_PAIR_INTERVAL_MS)
#define AUTH_GLOBAL_CAPACITY_MS (64u * AUTH_GLOBAL_INTERVAL_MS)

typedef struct {
    bool active;
    uint32_t source;
    char user[256];
    uint64_t updated_ms, seen_ms, credit_ms;
} AuthBucket;

static AuthBucket auth_sources[AUTH_SOURCE_BUCKETS];
static AuthBucket auth_pairs[AUTH_PAIR_BUCKETS];
static AuthBucket auth_global;
static AuthRateLimitStats auth_stats;

static void increment_auth_counter(uint64_t* value) {
    if (*value < UINT64_MAX) (*value)++;
}

AuthRateLimitStats auth_rate_limit_stats(void) {
    return auth_stats;
}

void record_auth_credential_failure(void) {
    increment_auth_counter(&auth_stats.credential_failures);
}

void reset_auth_rate_limit(void) {
    memset(auth_sources, 0, sizeof(auth_sources));
    memset(auth_pairs, 0, sizeof(auth_pairs));
    memset(&auth_global, 0, sizeof(auth_global));
    memset(&auth_stats, 0, sizeof(auth_stats));
}

static void refill_auth_bucket(AuthBucket* bucket, uint64_t now_ms, uint64_t capacity) {
    uint64_t elapsed = now_ms >= bucket->updated_ms ? now_ms - bucket->updated_ms : 0;
    uint64_t missing = capacity - bucket->credit_ms;
    bucket->credit_ms += elapsed < missing ? elapsed : missing;
    bucket->updated_ms = now_ms;
}

/* Refresh the idle timestamp only when an attempt is actually admitted, so a
   trickle of throttled requests cannot pin buckets forever. */
static void touch_auth_bucket(AuthBucket* bucket, uint64_t now_ms) {
    bucket->seen_ms = now_ms;
}

static AuthBucket* find_auth_bucket(AuthBucket* buckets, size_t count,
                                    uint32_t source, const char* user,
                                    uint64_t now_ms, uint64_t capacity) {
    AuthBucket* available = NULL;
    for (size_t i = 0; i < count; i++) {
        AuthBucket* bucket = &buckets[i];
        bool expired = bucket->active && now_ms >= bucket->seen_ms &&
                       now_ms - bucket->seen_ms >= AUTH_BUCKET_IDLE_MS;
        if (!bucket->active || expired) {
            if (!available) available = bucket;
        } else if (bucket->source == source && !strcmp(bucket->user, user)) {
            refill_auth_bucket(bucket, now_ms, capacity);
            return bucket;
        }
    }
    /* Never evict a live entry: source/account churn must not reset its budget. */
    if (!available) return NULL;
    memset(available, 0, sizeof(*available));
    available->active = true;
    available->source = source;
    memcpy(available->user, user, strlen(user) + 1);
    available->updated_ms = available->seen_ms = now_ms;
    available->credit_ms = capacity;
    return available;
}

bool admit_auth_attempt(uint32_t source, const char* user, uint64_t now_ms) {
    if (!user || strlen(user) >= sizeof(auth_pairs[0].user))
        return report_database_error("invalid authentication rate-limit identity");
    if (!auth_global.active) {
        auth_global.active = true;
        auth_global.updated_ms = now_ms;
        auth_global.credit_ms = AUTH_GLOBAL_CAPACITY_MS;
    }
    refill_auth_bucket(&auth_global, now_ms, AUTH_GLOBAL_CAPACITY_MS);
    AuthBucket* source_bucket = find_auth_bucket(
        auth_sources, AUTH_SOURCE_BUCKETS, source, "", now_ms, AUTH_SOURCE_CAPACITY_MS);
    if (!source_bucket) {
        increment_auth_counter(&auth_stats.source_saturated);
        return false;
    }
    if (source_bucket->credit_ms < AUTH_SOURCE_INTERVAL_MS) {
        increment_auth_counter(&auth_stats.source_throttled);
        return false;
    }
    if (auth_global.credit_ms < AUTH_GLOBAL_INTERVAL_MS) {
        increment_auth_counter(&auth_stats.global_throttled);
        return false;
    }
    AuthBucket* pair_bucket = find_auth_bucket(
        auth_pairs, AUTH_PAIR_BUCKETS, source, user, now_ms, AUTH_PAIR_CAPACITY_MS);
    if (!pair_bucket) {
        increment_auth_counter(&auth_stats.pair_saturated);
        return false;
    }
    if (pair_bucket->credit_ms < AUTH_PAIR_INTERVAL_MS) {
        increment_auth_counter(&auth_stats.pair_throttled);
        return false;
    }
    source_bucket->credit_ms -= AUTH_SOURCE_INTERVAL_MS;
    pair_bucket->credit_ms -= AUTH_PAIR_INTERVAL_MS;
    auth_global.credit_ms -= AUTH_GLOBAL_INTERVAL_MS;
    touch_auth_bucket(source_bucket, now_ms);
    touch_auth_bucket(pair_bucket, now_ms);
    touch_auth_bucket(&auth_global, now_ms);
    increment_auth_counter(&auth_stats.admitted);
    return true;
}
