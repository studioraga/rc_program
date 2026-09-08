/* ranked_cache.c
 *
 * Incremental ranked-cache implementation.
 *
 * Completed stages:
 *   Stage 0 - basic CacheEntry data model
 *   Stage 1 - deterministic database abstraction
 *   Stage 2 - simplest fixed-size cache using a linear array
 *   Stage 3 - explicit linear minimum-rank eviction path
 *   Stage 4 - complete cache_get() hit/miss path
 *   Stage 5 - assertions and cache invariants
 *   Stage 6 - identify the first bottleneck with lookup instrumentation
 *
 * Stage 2 intentionally uses linear scanning for:
 *   - lookup
 *   - duplicate detection before insert
 *   - minimum-rank search
 *
 * Stage 5 adds explicit structural invariant validation and development-time
 * assertions without changing the Stage 4 cache algorithm or complexity.
 *
 * Stage 6 does not optimize the cache. It instruments the existing linear
 * lookup path so the first bottleneck can be demonstrated with exact operation
 * counts rather than wall-clock timing.
 */

#include <stdio.h>
#include <stddef.h>
#include <assert.h>
#include <stdint.h>

#define MAX_CACHE_CAPACITY 100U

typedef unsigned long long CacheKey;
typedef long long Rank;

typedef struct {
    CacheKey key;
    unsigned long long value;
    Rank rank;
} CacheEntry;

/*
 * Simplest cache representation for Stage 2.
 *
 * entries  - fixed-size array that stores resident cache entries
 * size     - number of valid entries currently stored
 * capacity - maximum number of entries allowed for this cache instance
 */
typedef struct {
    CacheEntry entries[MAX_CACHE_CAPACITY];
    size_t size;
    size_t capacity;
} Cache;


/* ---------- Stage 6: lookup instrumentation ---------- */

/*
 * Exact counters for the existing linear key-lookup path.
 *
 * These counters are diagnostic only. They deliberately do not change the
 * lookup algorithm or the cache data structure.
 */
typedef struct {
    uint64_t lookup_calls;
    uint64_t key_comparisons;
    uint64_t lookup_hits;
    uint64_t lookup_misses;
} LookupStats;

static LookupStats g_lookup_stats;

void lookup_stats_reset(void)
{
    g_lookup_stats.lookup_calls = 0U;
    g_lookup_stats.key_comparisons = 0U;
    g_lookup_stats.lookup_hits = 0U;
    g_lookup_stats.lookup_misses = 0U;
}

LookupStats lookup_stats_snapshot(void)
{
    return g_lookup_stats;
}


/* ---------- Stage 5: structural invariants ---------- */

/*
 * Validate the internal structural invariants of a cache.
 *
 * Invariants checked:
 *   - cache pointer is non-NULL
 *   - configured capacity is in [1, MAX_CACHE_CAPACITY]
 *   - resident size never exceeds configured capacity
 *   - no two resident entries have the same key
 *
 * This function returns a status instead of asserting so tests can verify
 * that deliberately corrupted cache states are detected.
 *
 * Complexity:
 *   O(N^2) because duplicate-key validation compares resident pairs.
 *
 * This validation cost is for correctness/debug checking only and is not
 * part of the intended production cache algorithm.
 */
int cache_validate(const Cache *cache)
{
    size_t i;
    size_t j;

    if (cache == NULL) {
        return 0;
    }

    if (cache->capacity == 0U || cache->capacity > MAX_CACHE_CAPACITY) {
        return 0;
    }

    if (cache->size > cache->capacity) {
        return 0;
    }

    for (i = 0U; i < cache->size; ++i) {
        for (j = i + 1U; j < cache->size; ++j) {
            if (cache->entries[i].key == cache->entries[j].key) {
                return 0;
            }
        }
    }

    return 1;
}

/*
 * Development-time assertion wrapper.
 * Standard C assert() is active unless NDEBUG is defined at build time.
 */
void cache_assert_invariants(const Cache *cache)
{
    assert(cache_validate(cache));
}

/* ---------- Stage 1: deterministic database abstraction ---------- */

CacheEntry db_read_entry(CacheKey key)
{
    CacheEntry e;

    e.key = key;
    e.value = key * 100ULL;
    e.rank = (Rank)(key * 10ULL);

    return e;
}

/* ---------- Stage 2: simplest cache implementation ---------- */

/*
 * Initialize an empty cache.
 *
 * Returns:
 *   1 on success
 *   0 if capacity is invalid
 */
int cache_init(Cache *cache, size_t capacity)
{
    if (cache == NULL || capacity == 0U || capacity > MAX_CACHE_CAPACITY) {
        return 0;
    }

    cache->size = 0U;
    cache->capacity = capacity;
    cache_assert_invariants(cache);
    return 1;
}

/*
 * Check whether the cache has reached its configured capacity.
 *
 * Complexity:
 *   O(1)
 */
int cache_is_full(const Cache *cache)
{
    if (cache == NULL) {
        return 0;
    }

    cache_assert_invariants(cache);
    return cache->size >= cache->capacity;
}

/*
 * Find an entry by key using a linear scan.
 *
 * Returns:
 *   pointer to the matching entry on hit
 *   NULL on miss
 *
 * Complexity:
 *   O(N)
 */
CacheEntry *cache_lookup(Cache *cache, CacheKey key)
{
    size_t i;

    if (cache == NULL) {
        return NULL;
    }

    cache_assert_invariants(cache);
    ++g_lookup_stats.lookup_calls;

    for (i = 0U; i < cache->size; ++i) {
        ++g_lookup_stats.key_comparisons;

        if (cache->entries[i].key == key) {
            ++g_lookup_stats.lookup_hits;
            return &cache->entries[i];
        }
    }

    ++g_lookup_stats.lookup_misses;
    return NULL;
}

/*
 * Insert a new entry.
 *
 * Stage 2 policy:
 *   - reject duplicate keys
 *   - reject insertion if the cache is already full
 *
 * Returns:
 *   1 if inserted
 *   0 otherwise
 *
 * Complexity:
 *   O(N), because duplicate detection uses cache_lookup().
 */
int cache_insert(Cache *cache, CacheEntry entry)
{
    if (cache == NULL) {
        return 0;
    }

    cache_assert_invariants(cache);

    if (cache_is_full(cache)) {
        return 0;
    }

    if (cache_lookup(cache, entry.key) != NULL) {
        return 0;
    }

    cache->entries[cache->size] = entry;
    ++cache->size;

    cache_assert_invariants(cache);
    return 1;
}

/*
 * Find the array index of the entry having the minimum rank.
 *
 * Tie rule:
 *   if two entries have the same rank, the first one encountered
 *   in the array is selected.
 *
 * Returns:
 *   1 when an entry exists and writes its index to *min_index
 *   0 for an empty/invalid cache
 *
 * Complexity:
 *   O(N)
 */
int cache_find_min_rank_index(const Cache *cache, size_t *min_index)
{
    size_t i;
    size_t candidate;

    if (cache == NULL || min_index == NULL || cache->size == 0U) {
        return 0;
    }

    cache_assert_invariants(cache);
    candidate = 0U;

    for (i = 1U; i < cache->size; ++i) {
        if (cache->entries[i].rank < cache->entries[candidate].rank) {
            candidate = i;
        }
    }

    *min_index = candidate;
    return 1;
}

/* ---------- Stage 3: explicit linear minimum-rank eviction ---------- */

/*
 * Remove an entry when its array index is already known.
 *
 * The last valid resident entry is copied into the removed slot, so no
 * O(N) shift of the remaining array is required. Cache order has no
 * semantic meaning at this checkpoint.
 *
 * Returns:
 *   1 on successful removal
 *   0 for an invalid cache/index
 *
 * Complexity:
 *   O(1)
 */
int cache_remove_at(Cache *cache, size_t index, CacheEntry *removed_entry)
{
    if (cache == NULL || index >= cache->size) {
        return 0;
    }

    cache_assert_invariants(cache);

    if (removed_entry != NULL) {
        *removed_entry = cache->entries[index];
    }

    cache->entries[index] = cache->entries[cache->size - 1U];
    --cache->size;

    cache_assert_invariants(cache);
    return 1;
}

/*
 * Evict the minimum-ranked entry.
 *
 * Stage 3 composes two explicit operations:
 *   1. locate the minimum-ranked resident with a linear O(N) scan
 *   2. remove that known array slot in O(1)
 *
 * Overall complexity remains O(N), dominated by victim selection.
 */
int cache_evict_min(Cache *cache, CacheEntry *evicted_entry)
{
    size_t min_index;

    if (!cache_find_min_rank_index(cache, &min_index)) {
        return 0;
    }

    return cache_remove_at(cache, min_index, evicted_entry);
}


/* ---------- Stage 4: complete cache_get() path ---------- */

/*
 * Get an entry from the cache.
 *
 * Hit path:
 *   - linearly search the cache
 *   - return the resident entry
 *
 * Miss path:
 *   - read the entry through db_read_entry()
 *   - if the cache is full, evict the current minimum-rank resident
 *   - insert the fetched entry
 *   - return the newly resident entry
 *
 * Returns NULL only if cache state/operations prevent completion.
 *
 * Complexity with the current linear Stage 4 structures:
 *   hit:  O(N)
 *   miss: O(N) + DB-read cost
 *
 * The miss path can perform more than one linear pass. Optimization is
 * deliberately deferred to later stages.
 */
CacheEntry *cache_get(Cache *cache, CacheKey key)
{
    CacheEntry *found;
    CacheEntry fetched;

    if (cache == NULL) {
        return NULL;
    }

    cache_assert_invariants(cache);
    found = cache_lookup(cache, key);
    if (found != NULL) {
        return found;
    }

    fetched = db_read_entry(key);

    if (cache_is_full(cache)) {
        if (!cache_evict_min(cache, NULL)) {
            return NULL;
        }
    }

    if (!cache_insert(cache, fetched)) {
        return NULL;
    }

    return cache_lookup(cache, key);
}

/*
 * Print current cache contents as {key:rank, ...}.
 * Used only by the Stage 2 validation code.
 */
void cache_print(const Cache *cache)
{
    size_t i;

    printf("cache = {");

    if (cache != NULL) {
        for (i = 0U; i < cache->size; ++i) {
            printf("%llu:%lld",
                   cache->entries[i].key,
                   cache->entries[i].rank);

            if (i + 1U < cache->size) {
                printf(", ");
            }
        }
    }

    printf("}\n");
}

/*
 * Small PASS/FAIL helper for incremental validation.
 */
int check(int condition, const char *name)
{
    if (condition) {
        printf("[PASS] %s\n", name);
        return 1;
    }

    printf("[FAIL] %s\n", name);
    return 0;
}

/* ---------- Stage 6: deterministic bottleneck identification ---------- */

/*
 * Fill a cache directly with valid unique entries for lookup profiling.
 * This avoids cache_insert() because Stage 6 is measuring cache_lookup()
 * itself, not insertion's duplicate-check lookup.
 */
int stage6_fill_profile_cache(Cache *cache, size_t count)
{
    size_t i;

    if (!cache_init(cache, count)) {
        return 0;
    }

    for (i = 0U; i < count; ++i) {
        CacheKey key = (CacheKey)(i + 1U);

        cache->entries[i] = db_read_entry(key);
    }

    cache->size = count;
    cache_assert_invariants(cache);
    return 1;
}

/*
 * Profile one lookup and verify the exact number of key comparisons.
 */
int stage6_check_lookup_cost(Cache *cache,
                             CacheKey key,
                             uint64_t expected_comparisons,
                             int expect_hit,
                             const char *name)
{
    CacheEntry *entry;
    LookupStats stats;
    int passed;

    lookup_stats_reset();
    entry = cache_lookup(cache, key);
    stats = lookup_stats_snapshot();

    passed = (stats.lookup_calls == 1U &&
              stats.key_comparisons == expected_comparisons &&
              stats.lookup_hits == (expect_hit ? 1U : 0U) &&
              stats.lookup_misses == (expect_hit ? 0U : 1U) &&
              ((entry != NULL) == expect_hit));

    printf("[PROFILE] %-28s size=%zu comparisons=%llu result=%s\n",
           name,
           cache->size,
           (unsigned long long)stats.key_comparisons,
           entry != NULL ? "HIT" : "MISS");

    return check(passed, name);
}

int main(void)
{
    Cache cache;
    Cache invalid;
    Cache duplicate;
    Cache profile;
    CacheEntry *entry;
    LookupStats stats;
    size_t sizes[] = {1U, 10U, 25U, 50U, 100U};
    size_t i;
    int all_passed = 1;

    printf("=== Stage 6: identify first bottleneck ===\n");

    /* Stage 5 regression: normal cache behavior remains correct. */
    all_passed &= check(cache_init(&cache, 3U),
                        "initialize valid cache");
    all_passed &= check(cache_validate(&cache),
                        "fresh cache satisfies invariants");

    all_passed &= check(cache_get(&cache, 1ULL) != NULL,
                        "GET 1 succeeds");
    all_passed &= check(cache_get(&cache, 2ULL) != NULL,
                        "GET 2 succeeds");
    all_passed &= check(cache_get(&cache, 3ULL) != NULL,
                        "GET 3 succeeds");

    entry = cache_get(&cache, 2ULL);
    all_passed &= check(entry != NULL && entry->key == 2ULL,
                        "cache hit remains correct");

    entry = cache_get(&cache, 4ULL);
    all_passed &= check(entry != NULL && entry->key == 4ULL,
                        "full-cache miss remains correct");
    all_passed &= check(cache_validate(&cache),
                        "post-eviction cache satisfies invariants");

    /* Preserve Stage 5 invalid-state checks. */
    invalid = cache;
    invalid.size = invalid.capacity + 1U;
    all_passed &= check(!cache_validate(&invalid),
                        "validator detects size greater than capacity");

    invalid = cache;
    invalid.capacity = 0U;
    all_passed &= check(!cache_validate(&invalid),
                        "validator detects zero capacity");

    duplicate = cache;
    duplicate.entries[1].key = duplicate.entries[0].key;
    all_passed &= check(!cache_validate(&duplicate),
                        "validator detects duplicate resident keys");

    all_passed &= check(!cache_validate(NULL),
                        "validator rejects NULL cache");

    /*
     * Stage 6 experiment 1: within one 100-entry cache, compare the first,
     * middle, last, and missing-key cases. A linear scan should require
     * 1, 50, 100, and 100 key comparisons respectively.
     */
    all_passed &= check(stage6_fill_profile_cache(&profile, 100U),
                        "prepare 100-entry profiling cache");

    all_passed &= stage6_check_lookup_cost(&profile, 1ULL, 1U, 1,
                                           "first-entry lookup");
    all_passed &= stage6_check_lookup_cost(&profile, 50ULL, 50U, 1,
                                           "middle-entry lookup");
    all_passed &= stage6_check_lookup_cost(&profile, 100ULL, 100U, 1,
                                           "last-entry lookup");
    all_passed &= stage6_check_lookup_cost(&profile, 1000ULL, 100U, 0,
                                           "missing-entry lookup");

    /*
     * Stage 6 experiment 2: scale resident size while always requesting a
     * missing key. Exact comparison count must grow one-for-one with N.
     */
    printf("\nStage 6 lookup scaling (missing key):\n");
    printf("  size    key-comparisons\n");

    for (i = 0U; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        size_t n = sizes[i];

        all_passed &= check(stage6_fill_profile_cache(&profile, n),
                            "prepare scaling cache");

        lookup_stats_reset();
        entry = cache_lookup(&profile, 1000ULL);
        stats = lookup_stats_snapshot();

        printf("  %4zu    %llu\n",
               n,
               (unsigned long long)stats.key_comparisons);

        all_passed &= check(entry == NULL &&
                            stats.lookup_calls == 1U &&
                            stats.lookup_misses == 1U &&
                            stats.key_comparisons == (uint64_t)n,
                            "missing lookup comparisons equal cache size");
    }

    printf("\nStage 6 finding: cache_lookup() grows linearly with resident size.\n");
    printf("First bottleneck identified: O(N) key lookup.\n");
    printf("Stage 6 validation: %s\n",
           all_passed ? "PASS" : "FAIL");

    return all_passed ? 0 : 1;
}
