/* ranked_cache.c
 *
 * Incremental ranked-cache implementation.
 *
 * Completed stages:
 *   Stage 0 - basic CacheEntry data model
 *   Stage 1 - deterministic database abstraction
 *   Stage 2 - simplest fixed-size cache using a linear array
 *   Stage 3 - explicit linear minimum-rank eviction path
 *
 * Stage 2 intentionally uses linear scanning for:
 *   - lookup
 *   - duplicate detection before insert
 *   - minimum-rank search
 *
 * Stage 3 keeps the same linear-time eviction complexity while separating
 * victim selection from removal of a known array slot.
 */

#include <stdio.h>
#include <stddef.h>

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
    return cache != NULL && cache->size >= cache->capacity;
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

    for (i = 0U; i < cache->size; ++i) {
        if (cache->entries[i].key == key) {
            return &cache->entries[i];
        }
    }

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
    if (cache == NULL || cache_is_full(cache)) {
        return 0;
    }

    if (cache_lookup(cache, entry.key) != NULL) {
        return 0;
    }

    cache->entries[cache->size] = entry;
    ++cache->size;

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

    if (removed_entry != NULL) {
        *removed_entry = cache->entries[index];
    }

    cache->entries[index] = cache->entries[cache->size - 1U];
    --cache->size;

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

int main(void)
{
    Cache cache;
    CacheEntry evicted;
    CacheEntry *found;
    size_t min_index;
    int all_passed = 1;

    /*
     * Stage 2 manual-oracle entries.
     *
     * These values intentionally match the original problem example.
     * The Stage 1 db_read_entry() abstraction remains unchanged and is
     * not yet wired into cache hit/miss behavior at this checkpoint.
     */
    const CacheEntry e1 = {1ULL, 100ULL, 50LL};
    const CacheEntry e2 = {2ULL, 200ULL, 20LL};
    const CacheEntry e3 = {3ULL, 300ULL, 80LL};
    const CacheEntry e4 = {4ULL, 400ULL, 70LL};

    printf("=== Stage 3: linear minimum-rank eviction ===\n");

    all_passed &= check(cache_init(&cache, 3U),
                        "initialize cache with capacity 3");
    all_passed &= check(cache.size == 0U && cache.capacity == 3U,
                        "cache starts empty");

    /* INSERT: fill the cache to capacity. */
    all_passed &= check(cache_insert(&cache, e1), "insert key 1");
    all_passed &= check(cache_insert(&cache, e2), "insert key 2");
    all_passed &= check(cache_insert(&cache, e3), "insert key 3");
    cache_print(&cache);

    /* CAPACITY CHECK */
    all_passed &= check(cache_is_full(&cache),
                        "capacity check reports full");
    all_passed &= check(!cache_insert(&cache, e4),
                        "insert is rejected while cache is full");

    /* LOOKUP: one hit and one miss. */
    found = cache_lookup(&cache, 1ULL);
    all_passed &= check(found != NULL &&
                        found->value == 100ULL &&
                        found->rank == 50LL,
                        "linear lookup finds key 1");

    all_passed &= check(cache_lookup(&cache, 99ULL) == NULL,
                        "linear lookup reports missing key");

    /* MINIMUM-RANK SEARCH */
    all_passed &= check(cache_find_min_rank_index(&cache, &min_index),
                        "minimum-rank search succeeds");
    all_passed &= check(cache.entries[min_index].key == 2ULL &&
                        cache.entries[min_index].rank == 20LL,
                        "minimum rank is key 2 with rank 20");

    /* EVICTION */
    all_passed &= check(cache_evict_min(&cache, &evicted),
                        "evict minimum-ranked entry");
    all_passed &= check(evicted.key == 2ULL && evicted.rank == 20LL,
                        "evicted entry is key 2 rank 20");
    all_passed &= check(cache_lookup(&cache, 2ULL) == NULL,
                        "evicted key 2 is no longer present");
    all_passed &= check(!cache_is_full(&cache),
                        "cache is no longer full after eviction");

    /*
     * INSERT after eviction.
     * This completes the manual oracle:
     * resident keys are 1, 3, and 4.
     */
    all_passed &= check(cache_insert(&cache, e4),
                        "insert key 4 after eviction");
    all_passed &= check(cache.size == 3U,
                        "cache size returns to capacity");
    all_passed &= check(cache_lookup(&cache, 1ULL) != NULL &&
                        cache_lookup(&cache, 3ULL) != NULL &&
                        cache_lookup(&cache, 4ULL) != NULL,
                        "final cache contains keys 1, 3, and 4");

    cache_print(&cache);

    /* Stage 3 dedicated eviction edge cases. */
    {
        Cache empty_cache;
        Cache tie_cache;
        CacheEntry tie_evicted;
        const CacheEntry t1 = {10ULL, 1000ULL, 5LL};
        const CacheEntry t2 = {11ULL, 1100ULL, 5LL};

        all_passed &= check(cache_init(&empty_cache, 2U),
                            "initialize empty eviction-test cache");
        all_passed &= check(!cache_evict_min(&empty_cache, NULL),
                            "eviction from empty cache is rejected");
        all_passed &= check(!cache_remove_at(&cache, cache.size, NULL),
                            "removal rejects out-of-range index");

        all_passed &= check(cache_init(&tie_cache, 2U),
                            "initialize equal-rank test cache");
        all_passed &= check(cache_insert(&tie_cache, t1) &&
                            cache_insert(&tie_cache, t2),
                            "insert equal-rank entries");
        all_passed &= check(cache_evict_min(&tie_cache, &tie_evicted),
                            "evict from equal-rank cache");
        all_passed &= check(tie_evicted.key == 10ULL &&
                            tie_evicted.rank == 5LL,
                            "equal-rank tie evicts first encountered entry");
    }

    printf("Stage 3 validation: %s\n",
           all_passed ? "PASS" : "FAIL");

    return all_passed ? 0 : 1;
}
